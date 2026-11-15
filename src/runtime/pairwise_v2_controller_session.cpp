#include "runtime/pairwise_v2_controller_session.hpp"

#include "runtime/controller_ipc_adapters.hpp"

namespace epycbench::runtime::detail {
namespace {

using Failure = PairwiseV2ControllerSessionFailure;

auto close_endpoint(platform_linux::LocalIpcEndpoint& endpoint) noexcept -> bool {
    try {
        return endpoint.close().has_value();
    } catch (...) {
        return false;
    }
}

struct CleanupResult final {
    std::optional<platform_linux::ProcessExitStatus> status;
    bool clean_eof{};
    bool termination_sent{};
    bool kill_sent{};
    std::optional<Failure> primary;
    std::array<Failure, 8> secondary{};
    std::size_t secondary_count{};
};

void record(CleanupResult& result, Failure failure) noexcept {
    if (!result.primary) {
        result.primary = failure;
    } else if (result.secondary_count < result.secondary.size()) {
        result.secondary[result.secondary_count++] = failure;
    }
}

void record(PairwiseV2ControllerSessionError& error, Failure failure) noexcept {
    if (error.secondary_count < error.secondary.size())
        error.secondary[error.secondary_count++] = failure;
}

auto error_from(Failure primary, const CleanupResult& cleanup) noexcept
    -> PairwiseV2ControllerSessionError {
    PairwiseV2ControllerSessionError error{primary};
    if (cleanup.primary)
        record(error, *cleanup.primary);
    for (std::size_t index = 0; index < cleanup.secondary_count; ++index)
        record(error, cleanup.secondary[index]);
    error.process_status = cleanup.status;
    error.clean_eof = cleanup.clean_eof;
    error.termination_sent = cleanup.termination_sent;
    error.kill_sent = cleanup.kill_sent;
    return error;
}

auto error_from(const CleanupResult& cleanup) noexcept -> PairwiseV2ControllerSessionError {
    auto error = PairwiseV2ControllerSessionError{cleanup.primary.value_or(Failure::reap)};
    for (std::size_t index = 0; index < cleanup.secondary_count; ++index)
        record(error, cleanup.secondary[index]);
    error.process_status = cleanup.status;
    error.clean_eof = cleanup.clean_eof;
    error.termination_sent = cleanup.termination_sent;
    error.kill_sent = cleanup.kill_sent;
    return error;
}

auto retire(platform_linux::ChildProcess& child,
            platform_linux::LocalIpcEndpoint& control,
            platform_linux::LocalIpcEndpoint& events,
            std::chrono::milliseconds grace, ControllerClock clock,
            ControllerFatalInvariant fatal, bool allow_graceful_eof) noexcept -> CleanupResult {
    CleanupResult result;
    const auto control_closed = close_endpoint(control);
    if (!control_closed)
        record(result, Failure::endpoint_close);
    try {
        if (allow_graceful_eof && child.active() && control_closed) {
            auto eof_deadline = checked_deadline(clock, grace);
            if (eof_deadline) {
                DeadlineEventReader reader{events, clock, *eof_deadline,
                                           "pairwise_v2_retirement_timeout",
                                           ControllerInterruptionPolicy::stop_after_frame_progress};
                auto trailing = PairwiseMonitoringHostCapacityStartupAccess::read_frame(reader);
                if (trailing && !*trailing) {
                    result.clean_eof = true;
                    if (!close_endpoint(events))
                        record(result, Failure::endpoint_close);
                    auto waited = child.wait();
                    if (!waited)
                        fatal.unreapable(fatal.context);
                    result.status = std::move(*waited);
                    return result;
                }
                record(result, trailing ? Failure::trailing_event : Failure::unexpected_eof);
            } else {
                record(result, Failure::deadline);
            }
        }
        if (!close_endpoint(events))
            record(result, Failure::endpoint_close);
        if (child.active()) {
            auto observed = child.try_wait();
            if (observed && *observed)
                result.status = std::move(**observed);
            else if (!observed &&
                     observed.error().context.value("classification", "") != "interrupted")
                record(result, Failure::reap);
        }
        if (child.active()) {
            result.termination_sent = true;
            if (!child.send_signal(platform_linux::ProcessSignal::terminate))
                record(result, Failure::termination);
            auto deadline = checked_deadline(clock, grace);
            if (!deadline)
                record(result, Failure::deadline);
            while (deadline && child.active() && clock.now(clock.context) < *deadline) {
                auto observed = child.try_wait();
                if (observed && *observed) {
                    result.status = std::move(**observed);
                    break;
                }
                if (!observed &&
                    observed.error().context.value("classification", "") != "interrupted") {
                    record(result, Failure::reap);
                    break;
                }
            }
        }
        if (child.active()) {
            result.kill_sent = true;
            if (!child.send_signal(platform_linux::ProcessSignal::kill))
                record(result, Failure::termination);
        }
        if (child.active()) {
            auto waited = child.wait();
            if (!waited)
                fatal.unreapable(fatal.context);
            result.status = std::move(*waited);
        }
    } catch (...) {
        fatal.unreapable(fatal.context);
    }
    return result;
}

class EmergencyCleanup final {
  public:
    EmergencyCleanup(platform_linux::ChildProcess& child,
                     platform_linux::LocalIpcEndpoint& control,
                     platform_linux::LocalIpcEndpoint& events,
                     std::chrono::milliseconds grace, ControllerClock clock,
                     ControllerFatalInvariant fatal) noexcept
        : child_{&child}, control_{&control}, events_{&events}, grace_{grace},
          clock_{clock}, fatal_{fatal} {}
    EmergencyCleanup(const EmergencyCleanup&) = delete;
    auto operator=(const EmergencyCleanup&) -> EmergencyCleanup& = delete;
    ~EmergencyCleanup() noexcept {
        if (armed_ && child_->active())
            static_cast<void>(retire(*child_, *control_, *events_, grace_, clock_,
                                     fatal_, false));
    }
    void disarm() noexcept { armed_ = false; }

  private:
    platform_linux::ChildProcess* child_;
    platform_linux::LocalIpcEndpoint* control_;
    platform_linux::LocalIpcEndpoint* events_;
    std::chrono::milliseconds grace_;
    ControllerClock clock_;
    ControllerFatalInvariant fatal_;
    bool armed_{true};
};

} // namespace

auto PairwiseV2ControllerSessionAccess::run(
    const platform_linux::ChildSpawnRequest& spawn_request,
    AuthoredPairwiseHostCapacityOffer offer,
    const PairwiseHostCapacityRequestTranscript& request,
    const PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority& controller,
    PairwiseV2ControllerSessionConfig config, ControllerDependencies dependencies,
    std::optional<PairwiseV2CancellationReason> cancellation) noexcept
    -> std::expected<PairwiseV2ControllerProof, PairwiseV2ControllerSessionError> {
    try {
        if (spawn_request.arguments.size() != 2U ||
            spawn_request.arguments[0] != spawn_request.executable ||
            spawn_request.arguments[1] != "__runner" ||
            config.startup_timeout.count() < 0 || config.termination_grace.count() < 0 ||
            !dependencies.channels.create || !dependencies.spawner.spawn ||
            !dependencies.clock.now || !dependencies.fatal.unreapable)
            return std::unexpected{Failure::invalid_input};
        auto deadline = checked_deadline(dependencies.clock, config.startup_timeout);
        if (!deadline)
            return std::unexpected{Failure::deadline};
        auto channels_result = dependencies.channels.create(dependencies.channels.context);
        if (!channels_result)
            return std::unexpected{Failure::channel_creation};
        auto channels = std::move(*channels_result);
        auto child_result = dependencies.spawner.spawn(
            dependencies.spawner.context, spawn_request, channels.runner);
        if (!child_result) {
            PairwiseV2ControllerSessionError error{Failure::spawn};
            if (!close_endpoint(channels.runner.event_writer)) record(error, Failure::endpoint_close);
            if (!close_endpoint(channels.runner.control_reader)) record(error, Failure::endpoint_close);
            if (!close_endpoint(channels.controller.control_writer)) record(error, Failure::endpoint_close);
            if (!close_endpoint(channels.controller.event_reader)) record(error, Failure::endpoint_close);
            return std::unexpected{error};
        }
        auto child = std::move(*child_result);
        EmergencyCleanup emergency{child, channels.controller.control_writer,
                                   channels.controller.event_reader,
                                   config.termination_grace, dependencies.clock,
                                   dependencies.fatal};
        std::optional<Failure> primary;
        std::array<Failure, 2> early_secondary{};
        std::size_t early_secondary_count{};
        if (!close_endpoint(channels.runner.event_writer))
            primary = Failure::endpoint_close;
        if (!close_endpoint(channels.runner.control_reader)) {
            if (!primary) primary = Failure::endpoint_close;
            else early_secondary[early_secondary_count++] = Failure::endpoint_close;
        }

        std::optional<PairwiseV2ControllerProofKind> kind;
        std::optional<PairwiseHostCapacityRejectionReason> rejection;
        DeadlineControlWriter writer{channels.controller.control_writer, dependencies.clock,
                                     *deadline, "pairwise_v2_startup_timeout",
                                     ControllerInterruptionPolicy::stop_after_frame_progress};
        std::optional<PairwiseV2ControlWriterState> control;
        if (!primary) {
            auto control_result = PairwiseV2PhaseAProtocolAccess::write_startup_offer(
                writer, std::move(offer));
            if (!control_result)
                primary = Failure::offer_write;
            else {
                control.emplace(std::move(*control_result));
                writer.reset_progress();
            }
        }
        if (!primary) {
            auto prepared = PairwiseV2PhaseAProtocolAccess::prepare_request_write(
                std::move(*control), request);
            if (!prepared)
                primary = Failure::request_write;
            else {
                auto committed = PairwiseV2PhaseAProtocolAccess::commit_control_write(
                    writer, std::move(*prepared));
                control.reset();
                if (!committed)
                    primary = Failure::request_write;
                else {
                    control.emplace(std::move(*committed));
                    writer.reset_progress();
                }
            }
        }
        if (!primary && cancellation) {
            auto prepared = PairwiseV2PhaseAProtocolAccess::prepare_cancel_write(
                std::move(*control), *cancellation);
            if (!prepared)
                primary = Failure::cancel_write;
            else {
                auto committed = PairwiseV2PhaseAProtocolAccess::commit_control_write(
                    writer, std::move(*prepared));
                control.reset();
                if (!committed)
                    primary = Failure::cancel_write;
                else
                    control.emplace(std::move(*committed));
            }
        }
        if (!primary) {
            DeadlineEventReader reader{channels.controller.event_reader, dependencies.clock,
                                       *deadline, "pairwise_v2_startup_timeout",
                                       ControllerInterruptionPolicy::stop_after_frame_progress};
            auto startup = PairwiseV2PhaseAProtocolAccess::read_startup_outcome(
                reader, std::move(*control), controller);
            if (!startup) {
                primary = Failure::startup_read;
            } else if (auto* rejected =
                           std::get_if<PairwiseV2ControllerRejected>(&*startup)) {
                kind = PairwiseV2ControllerProofKind::rejected;
                rejection = rejected->reason;
            } else {
                reader.reset_progress();
                auto accepted = std::get<PairwiseV2ControllerAcceptedHandoff>(
                    std::move(*startup));
                auto body = PairwiseMonitoringHostCapacityStartupAccess::read_frame(reader);
                if (!body || !*body ||
                    !PairwiseV2PhaseAProtocolAccess::read_accepted(accepted.event, **body)) {
                    primary = Failure::accepted_read;
                } else if (cancellation) {
                    reader.reset_progress();
                    body = PairwiseMonitoringHostCapacityStartupAccess::read_frame(reader);
                    if (!body || !*body ||
                        !PairwiseV2PhaseAProtocolAccess::read_cancelled(accepted.event, **body))
                        primary = Failure::cancelled_read;
                    else
                        kind = PairwiseV2ControllerProofKind::cancelled;
                } else {
                    kind = PairwiseV2ControllerProofKind::accepted_prepared_non_success;
                }
            }
        }

        auto cleanup = retire(child, channels.controller.control_writer,
                              channels.controller.event_reader, config.termination_grace,
                              dependencies.clock, dependencies.fatal,
                              kind.has_value() && !primary.has_value());
        if (!child.active())
            emergency.disarm();
        if (primary) {
            auto error = error_from(*primary, cleanup);
            for (std::size_t index = 0; index < early_secondary_count; ++index)
                record(error, early_secondary[index]);
            return std::unexpected{error};
        }
        if (cleanup.primary)
            return std::unexpected{error_from(cleanup)};
        if (!cleanup.clean_eof)
            return std::unexpected{error_from(Failure::unexpected_eof, cleanup)};
        if (!cleanup.status)
            return std::unexpected{error_from(Failure::reap, cleanup)};
        if (const auto* exited = std::get_if<platform_linux::ProcessExited>(&*cleanup.status);
            exited && exited->code != 0)
            return std::unexpected{error_from(Failure::process_nonzero, cleanup)};
        if (std::holds_alternative<platform_linux::ProcessSignaled>(*cleanup.status))
            return std::unexpected{error_from(Failure::process_signaled, cleanup)};
        if (cleanup.termination_sent || cleanup.kill_sent)
            return std::unexpected{error_from(Failure::termination, cleanup)};
        if (!kind)
            return std::unexpected{Failure::accepted_read};
        return PairwiseV2ControllerProof{*kind, rejection, std::move(*cleanup.status),
                                         cleanup.termination_sent, cleanup.kill_sent};
    } catch (const std::exception&) {
        return std::unexpected{Failure::contained_exception};
    } catch (...) {
        return std::unexpected{Failure::contained_unknown_failure};
    }
}

} // namespace epycbench::runtime::detail
