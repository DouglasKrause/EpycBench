#include "runtime/pairwise_v2_synthetic_lifecycle.hpp"
#include "runtime/pairwise_v2_lifecycle_facts.hpp"
#include "runtime/allocation_free_json_retirement.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::runtime::detail {

// Only this orchestrator reserves cancellation alongside the sealed event role.
// The private terminal choice below permanently removes the reservation before
// starting success transmission. The runner never receives a clock.
struct PairwiseV2SyntheticProtocolAccess final {
    static auto bind(PairwiseV2ControllerResources& controller,
                     PairwiseV2RunnerResources& runner) noexcept -> bool {
        if (controller.identity() != runner.identity() ||
            pairwise_v2_resource_identity_commitment(controller.identity()) !=
                controller.authenticated_identity_ ||
            pairwise_v2_resource_identity_commitment(runner.identity()) !=
                runner.authenticated_identity_ ||
            controller.phase_a.control.phase_ !=
                PairwiseV2ControlWriterState::Phase::requested_acknowledged ||
            runner.control.phase_ != PairwiseV2ControlReaderState::Phase::requested ||
            controller.phase_a.event.phase_ != PairwiseV2EventReaderState::Phase::accepted ||
            runner.events.phase_ != PairwiseV2EventWriterState::Phase::accepted)
            return false;
        const auto pair = controller.identity().pair;
        const PairwiseV2PairIdentity expected{pair.first, pair.second};
        if (controller.phase_a.control.identity_ != expected || !runner.control.identity_ ||
            *runner.control.identity_ != expected ||
            controller.phase_a.event.identity_ != expected || runner.events.identity_ != expected)
            return false;
        controller.phase_a.control.phase_ = PairwiseV2ControlWriterState::Phase::invalid;
        runner.control.phase_ = PairwiseV2ControlReaderState::Phase::invalid;
        return true;
    }
};

namespace {
using Kind = SyntheticFailureKind;
using Stage = SyntheticStage;
using Phase = SyntheticPhase;
using Progress = PreparedTransportProgress;
struct StopIo final {};

struct FixedFrame final {
    std::array<std::byte, 1024U> bytes{};
    std::size_t size{4U};
    auto append(std::string_view text) noexcept -> bool {
        if (text.size() > bytes.size() - size)
            return false;
        for (const auto character : text)
            bytes[size++] = std::byte{static_cast<unsigned char>(character)};
        return true;
    }
    auto number(std::uint64_t value) noexcept -> bool {
        std::array<char, 20U> buffer{};
        const auto [end, error] =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return error == std::errc{} &&
               append({buffer.data(), static_cast<std::size_t>(end - buffer.data())});
    }
    void complete() noexcept {
        const auto length = size - 4U;
        for (unsigned index{}; index < 4U; ++index)
            bytes[index] = std::byte{static_cast<unsigned char>(length >> (24U - 8U * index))};
    }
    auto view() const noexcept -> std::span<const std::byte> { return {bytes.data(), size}; }
};
constexpr std::array reasons{"user_interrupt", "controller_shutdown", "controller_timeout"};
constexpr std::array phases{"startup", "admission", "service", "execution", "cleanup"};
auto cancellation_frame(PairwiseV2PairIdentity pair, std::size_t reason,
                        std::optional<std::size_t> phase) noexcept -> FixedFrame {
    FixedFrame frame;
    const auto put = [&](std::string_view text) { return frame.append(text); };
    bool valid = put(R"({"payload":{"first_request_id":")") && frame.number(pair.first.value()) &&
                 put(R"(",)");
    if (phase)
        valid = valid && put(R"("phase":")") && put(phases[*phase]) && put(R"(",)");
    valid = valid && put(R"("reason":")") && put(reasons[reason]) &&
            put(R"(","second_request_id":")") && frame.number(pair.second.value()) &&
            put(R"("},"payload_profile":")") &&
            put(phase ? pairwise_cancelled_profile : pairwise_cancel_profile) &&
            put(R"(","protocol":"epycbench.controller-runner-ipc/v2","request_id":")") &&
            frame.number(pair.first.value()) && put(R"(","sequence":"1","type":")") &&
            put(phase ? "pairwise_cancelled" : "cancel_pairwise") && put(R"("})");
    if (!valid)
        frame.size = 0U;
    else
        frame.complete();
    return frame;
}
auto error_classification(const core::Error& error) noexcept -> std::string_view {
    if (!error.context.is_object())
        return {};
    const auto found = error.context.find("classification");
    return found != error.context.end() && found->is_string()
               ? std::string_view{found->get_ref<const std::string&>()}
               : std::string_view{};
}
struct FramePosition final {
    std::array<std::byte, 4U> prefix{};
    std::size_t progress{}, target{4U}, count{};
    void consume(std::span<const std::byte> bytes) noexcept {
        for (const auto byte : bytes) {
            if (progress < 4U)
                prefix[progress] = byte;
            ++progress;
            if (progress == 4U) {
                std::uint32_t length{};
                for (const auto part : prefix)
                    length = (length << 8U) | std::to_integer<unsigned>(part);
                target = 4U + static_cast<std::size_t>(length);
            }
            if (progress == target) {
                ++count;
                progress = 0U;
                target = 4U;
            }
        }
    }
};
auto transport_stage(PreparedTransportStage stage) noexcept -> Stage {
    constexpr std::array stages{Stage::manifest, Stage::chunks, Stage::completion, Stage::eof};
    return stages[static_cast<std::size_t>(stage)];
}
constexpr std::array control_stages{Stage::host_offer, Stage::capacity_offer, Stage::prepared_offer,
                                    Stage::request};
constexpr std::array event_stages{Stage::host_ack, Stage::capacity_ack, Stage::prepared_ack,
                                  Stage::accepted};
} // namespace

struct PairwiseV2SyntheticLifecycle::Storage final {
    PairwiseV2ComposedStartupController startup;
    wire_v1::detail::StartupSessionId session;
    PairwiseV2PairIdentity pair;
    RunnerContext runner_context;
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations runner_authority;
    SyntheticLifecycleConfig config;
    SyntheticProcessOperations operations;
    SyntheticBackendPreparation fixture;
    PairwiseV2FakeEvidenceBackend backend{};
    SyntheticChannels channels{};
    std::optional<SyntheticChild> child;
    std::optional<PairwiseV2ActiveEvidenceRunner> runner;
    std::optional<PairwiseV2ActiveEvidenceController> controller;
    SyntheticLifecycleOutcome outcome;
    SyntheticCaptureOwner capture;
    std::array<FixedFrame, 3U> cancel_frames;
    std::array<FixedFrame, 15U> cancelled_frames;
    std::array<std::byte, 1024U> receive{};
    std::array<bool, 6U> closed{};
    std::uint8_t reported_cleanup_sources{};
    std::optional<std::int64_t> last_now;
    std::int64_t deadline{};
    Phase phase{Phase::startup};
    Stage stage{Stage::preparation};
    enum class State : unsigned char { prepared, starting, ready, retired, consumed } state{};
    bool have_deadline{}, forced{}, bound_cancel{}, terminal_available{true}, cancel_attempted{};
    bool exit_failure_recorded{}, grace_expired{}, termination_expired{}, reap_attempted{},
        child_stopped{}, flushing_cancel{};
    FramePosition control_position, runner_control_position, event_position,
        controller_event_position;

    Storage(PairwiseV2ComposedStartupController prepared, wire_v1::detail::StartupSessionId id,
            PairwiseV2PairIdentity identity, RunnerContext context,
            PairwiseMonitoringHostCapacityStartupAccess::LowerOperations authority,
            SyntheticLifecycleConfig limits, SyntheticProcessOperations dependencies,
            SyntheticBackendPreparation preparation) noexcept
        : startup{std::move(prepared)}, session{std::move(id)}, pair{identity},
          runner_context{std::move(context)}, runner_authority{authority}, config{limits},
          operations{dependencies}, fixture{preparation} {
        for (std::size_t reason{}; reason < reasons.size(); ++reason) {
            cancel_frames[reason] = cancellation_frame(pair, reason, std::nullopt);
            for (std::size_t position{}; position < phases.size(); ++position)
                cancelled_frames[reason * phases.size() + position] =
                    cancellation_frame(pair, reason, position);
        }
    }
    void mark(SyntheticLifecycleMark value) noexcept {
        if (capture.facts) capture.facts->mark(value);
    }
    void first_mark(SyntheticLifecycleMark value) noexcept {
        if (capture.facts && capture.facts->ordinal(value) == 0U) capture.facts->mark(value);
    }
    void cancellation_fact() noexcept {
        if (capture.facts && !capture.facts->cancellation && outcome.cancellation) {
            capture.facts->cancellation = outcome.cancellation;
            mark(SyntheticLifecycleMark::cancellation_decided);
        }
    }
    void record(Kind kind, Stage at,
                std::optional<PairwiseV2ActiveEvidenceFailure> evidence = std::nullopt) noexcept {
        const SyntheticFailure value{kind, at, phase, evidence};
        if (!outcome.primary) {
            outcome.primary = value;
            if (capture.facts) capture.facts->primary = value;
            mark(SyntheticLifecycleMark::primary_failure);
        }
        if (kind == Kind::clock_regression || kind == Kind::clock_overflow)
            first_mark(SyntheticLifecycleMark::clock_failed);
        if (kind == Kind::callback) first_mark(SyntheticLifecycleMark::callback_failed);
        if (kind == Kind::cancel_write) first_mark(SyntheticLifecycleMark::cancel_write_failed);
        if (kind == Kind::cancel_response) first_mark(SyntheticLifecycleMark::cancel_response_failed);
        if (kind == Kind::timeout)
            outcome.timeout_phases |= static_cast<std::uint8_t>(1U << static_cast<unsigned>(phase));
        if (kind == Kind::clock_regression || kind == Kind::clock_overflow)
            outcome.clock_failed = true;
        if (kind == Kind::callback)
            outcome.callback_failed = true;
        if (kind == Kind::cancel_write)
            outcome.cancel_write_failed = true;
        if (kind == Kind::cancel_response)
            outcome.cancel_response_failed = true;
    }
    void cleanup_failure(Kind kind, Stage at, SyntheticCleanupSource source) noexcept {
        const auto bit = static_cast<std::uint8_t>(1U << static_cast<unsigned>(source));
        if ((reported_cleanup_sources & bit) != 0U)
            return;
        reported_cleanup_sources |= bit;
        if (!outcome.primary)
            record(kind, at);
        else {
            outcome.secondary[outcome.secondary_count++] = {source,
                                                            {kind, at, phase, std::nullopt}};
            if (capture.facts) {
                capture.facts->secondary[capture.facts->secondary_count++] = outcome.secondary[outcome.secondary_count - 1U];
                mark(static_cast<SyntheticLifecycleMark>(
                    static_cast<unsigned>(SyntheticLifecycleMark::secondary_backend_retirement) +
                    static_cast<unsigned>(source)));
            }
        }
    }
    auto time() noexcept -> std::optional<std::int64_t> {
        try {
            const auto now = operations.now(operations.context);
            if (now < 0 || (last_now && now < *last_now)) {
                record(Kind::clock_regression, stage);
                forced = true;
                return {};
            }
            last_now = now;
            return now;
        } catch (...) {
            record(Kind::callback, stage);
            forced = true;
            return {};
        }
    }
    auto begin(Phase next, std::int64_t now) noexcept -> bool {
        phase = next;
        const std::array durations{config.startup, config.execution, config.cancellation,
                                   config.completion, config.termination};
        const auto duration = durations[static_cast<std::size_t>(next)];
        if (duration > std::numeric_limits<std::int64_t>::max() - now) {
            record(Kind::clock_overflow, stage);
            forced = true;
            return false;
        }
        deadline = now + duration;
        have_deadline = true;
        if (capture.facts) {
            capture.facts->deadlines[static_cast<std::size_t>(next)] = {now, duration, deadline, 0};
            mark(static_cast<SyntheticLifecycleMark>(
                static_cast<unsigned>(SyntheticLifecycleMark::startup_begin) +
                2U * static_cast<unsigned>(next)));
        }
        return true;
    }
    auto point(Stage at, bool external = true) noexcept -> bool {
        stage = at;
        bool decided{};
        if (external && !outcome.cancellation && operations.cancellation) {
            try {
                const auto reason = operations.cancellation(operations.context);
                if (reason) {
                    if (static_cast<std::size_t>(*reason) >= reasons.size()) {
                        record(Kind::callback, at);
                        forced = true;
                        return false;
                    }
                    outcome.cancellation = *reason;
                    cancellation_fact();
                    record(Kind::cancelled, at);
                    decided = true;
                }
            } catch (...) {
                record(Kind::callback, at);
                forced = true;
                return false;
            }
        }
        const auto now = time();
        if (!now)
            return false;
        if (decided && phase != Phase::termination && !begin(Phase::cancellation, *now))
            return false;
        if (have_deadline && *now >= deadline) {
            if (capture.facts) {
                const auto expired = static_cast<SyntheticLifecycleMark>(
                    static_cast<unsigned>(SyntheticLifecycleMark::startup_expiry) +
                    2U * static_cast<unsigned>(phase));
                if (capture.facts->ordinal(expired) == 0U) {
                    capture.facts->deadlines[static_cast<std::size_t>(phase)].expiry = *now;
                    mark(expired);
                }
            }
            if (phase == Phase::cancellation) {
                if (!grace_expired) {
                    record(Kind::timeout, at);
                    grace_expired = true;
                }
                forced = true;
            } else if (phase == Phase::termination) {
                if (!termination_expired) {
                    record(Kind::timeout, at);
                    termination_expired = true;
                }
                forced = true;
            } else {
                record(Kind::timeout, at);
                if (!outcome.cancellation)
                    outcome.cancellation = PairwiseV2CancellationReason::controller_timeout;
                cancellation_fact();
                if (!begin(Phase::cancellation, *now))
                    return false;
            }
        }
        return !forced &&
               (phase == Phase::termination || !outcome.cancellation || outcome.request_complete);
    }
    auto ready(SyntheticIo io, Stage at) noexcept -> bool {
        if (!point(at))
            return false;
        try {
            const auto value = operations.ready(operations.context, io);
            if (!value) {
                record(Kind::callback, at);
                forced = true;
                return false;
            }
            if (!point(at))
                return false;
            return *value;
        } catch (...) {
            record(Kind::callback, at);
            forced = true;
            return false;
        }
    }
    void close(SyntheticEndpoint endpoint, Stage at) noexcept {
        const auto index = static_cast<std::size_t>(endpoint);
        if (closed[index])
            return;
        closed[index] = true;
        const auto failed = [&](Kind kind) {
            using S = SyntheticCleanupSource;
            switch (endpoint) {
            case SyntheticEndpoint::runner_control:
                cleanup_failure(kind, at, S::runner_control);
                break;
            case SyntheticEndpoint::runner_event:
                cleanup_failure(kind, at, S::runner_event);
                break;
            case SyntheticEndpoint::controller_control:
                cleanup_failure(kind, at, S::controller_control);
                break;
            case SyntheticEndpoint::controller_event:
                cleanup_failure(kind, at, S::controller_event);
                break;
            default:
                outcome.parent_close_failures |=
                    endpoint == SyntheticEndpoint::parent_runner_event ? 1U : 2U;
                record(kind, at);
                break; // allocating startup parent-copy close
            }
        };
        static_cast<void>(point(at));
        mark(static_cast<SyntheticLifecycleMark>(
            static_cast<unsigned>(SyntheticLifecycleMark::parent_runner_control_close_attempt) +
            2U * static_cast<unsigned>(endpoint)));
        try {
            const auto result = operations.close(operations.context, endpoint);
            if (capture.facts) capture.facts->closes[index] = result ? SyntheticCapturedReturn::success : SyntheticCapturedReturn::failed;
            mark(static_cast<SyntheticLifecycleMark>(
                static_cast<unsigned>(SyntheticLifecycleMark::parent_runner_control_close_return) +
                2U * static_cast<unsigned>(endpoint)));
            if (!result) failed(Kind::close);
        } catch (...) {
            if (capture.facts) capture.facts->closes[index] = SyntheticCapturedReturn::exception;
            mark(static_cast<SyntheticLifecycleMark>(
                static_cast<unsigned>(SyntheticLifecycleMark::parent_runner_control_close_return) +
                2U * static_cast<unsigned>(endpoint)));
            failed(Kind::callback);
        }

        static_cast<void>(point(at));
    }
    void close_runner() noexcept {
        close(SyntheticEndpoint::runner_event, Stage::cleanup);
        close(SyntheticEndpoint::runner_control, Stage::cleanup);
    }
    void close_controller() noexcept {
        close(SyntheticEndpoint::controller_control, Stage::cleanup);
        close(SyntheticEndpoint::controller_event, Stage::cleanup);
    }
    auto transfer(const FixedFrame& frame, ipc::ByteWriter* writer, ipc::ByteReader* reader,
                  SyntheticIo io, Kind failure) noexcept -> bool {
        std::size_t progress{};
        while (progress < frame.size) {
            if (!ready(io, stage)) {
                if (forced || !point(stage))
                    return false;
                continue;
            }
            try {
                auto transferred =
                    writer ? writer->write(frame.view().subspan(progress))
                           : reader->read(
                                 std::span<std::byte>{receive}.first(frame.size).subspan(progress));
                if (!transferred) {
                    const auto category = error_classification(transferred.error());
                    const bool retry =
                        category == "would_block" || (category == "interrupted" && progress == 0U);
                    retire_json(transferred.error().context);
                    if (retry)
                        continue;
                    record(failure, stage);
                    forced = true;
                    return false;
                }
                if (*transferred == 0U || *transferred > frame.size - progress) {
                    record(failure, stage);
                    forced = true;
                    return false;
                }
                progress += *transferred;
                if (writer && progress == frame.size) {
                    if (io == SyntheticIo::cancel_write) mark(SyntheticLifecycleMark::cancel_written);
                    if (io == SyntheticIo::cancelled_write) {
                        if (capture.facts) capture.facts->cancelled_written_phase = outcome.activated
                            ? SyntheticCancelledPhase::execution : SyntheticCancelledPhase::admission;
                        mark(SyntheticLifecycleMark::cancelled_frame_written);
                    }
                }
                if (!point(stage))
                    return false;
            } catch (...) {
                record(Kind::callback, stage);
                forced = true;
                return false;
            }
        }
        if (reader && !std::equal(frame.view().begin(), frame.view().end(), receive.begin())) {
            record(failure, stage);
            forced = true;
            return false;
        }
        return true;
    }
    auto send_cancel() noexcept -> bool {
        if (!outcome.cancellation || !outcome.request_complete || outcome.cancel_sent)
            return true;
        if (cancel_attempted)
            return false;
        cancel_attempted = true;
        flushing_cancel = true;
        const auto& frame = cancel_frames[static_cast<std::size_t>(*outcome.cancellation)];
        const auto sent = transfer(frame, channels.controller_control, nullptr,
                                   SyntheticIo::cancel_write, Kind::cancel_write);
        flushing_cancel = false;
        outcome.cancel_sent = sent;
        return sent;
    }
    struct StartupIo final : ipc::ByteWriter, ipc::ByteReader {
        Storage& owner;
        SyntheticIo direction;
        ipc::ByteWriter* writer;
        ipc::ByteReader* reader;
        FramePosition& position;
        StartupIo(Storage& state, SyntheticIo io, ipc::ByteWriter* out, ipc::ByteReader* in,
                  FramePosition& frame)
            : owner{state}, direction{io}, writer{out}, reader{in}, position{frame} {}
        auto current_stage() const noexcept -> Stage {
            const bool control =
                direction == SyntheticIo::control_write || direction == SyntheticIo::control_read;
            return (control ? control_stages
                            : event_stages)[std::min(position.count, std::size_t{3U})];
        }
        auto invoke(std::span<const std::byte> input, std::span<std::byte> output)
            -> std::expected<std::size_t, core::Error> {
            const auto at = current_stage();
            for (;;) {
                if (!owner.ready(direction, at)) {
                    if (owner.forced || !owner.point(at))
                        throw StopIo{};
                    continue;
                }
                if (owner.outcome.cancellation && !owner.flushing_cancel && !owner.send_cancel())
                    throw StopIo{};
                // The buffered writer may offer all four frames at once. Never
                // hide a frame boundary from controller deadline/cancel checks.
                const auto offered =
                    writer
                        ? input.first(std::min(input.size(), position.target - position.progress))
                        : input;
                auto value = writer ? writer->write(offered) : reader->read(output);
                if (!value) {
                    const auto kind = error_classification(value.error());
                    const bool retry =
                        kind == "would_block" || (kind == "interrupted" && position.progress == 0U);
                    retire_json(value.error().context);
                    if (retry)
                        continue;
                    owner.record(Kind::protocol, at);
                    throw StopIo{};
                }
                const auto requested = writer ? offered.size() : output.size();
                if (*value > requested || (writer && *value == 0U)) {
                    owner.record(Kind::protocol, at);
                    throw StopIo{};
                }
                position.consume(writer ? input.first(*value)
                                        : std::span<const std::byte>{output}.first(*value));
                if (direction == SyntheticIo::control_write && position.count == 4U &&
                    !owner.outcome.request_complete) {
                    owner.outcome.request_complete = true;
                    owner.mark(SyntheticLifecycleMark::request_written);
                }
                if (!owner.point(at))
                    throw StopIo{};
                return *value;
            }
        }
        auto write(std::span<const std::byte> bytes)
            -> std::expected<std::size_t, core::Error> override {
            return invoke(bytes, {});
        }
        auto read(std::span<std::byte> bytes) -> std::expected<std::size_t, core::Error> override {
            return invoke({}, bytes);
        }
    };
    struct EvidenceWriter final : ipc::ByteWriter {
        Storage& owner;
        explicit EvidenceWriter(Storage& value) noexcept : owner{value} {}
        auto write(std::span<const std::byte> bytes)
            -> std::expected<std::size_t, core::Error> override {
            try {
                auto written = owner.channels.runner_event->write(bytes);
                if (written) {
                    if (*written > 0U)
                        owner.terminal_available = false;
                    if (*written > 0U && *written <= bytes.size() && !owner.outcome.success_started) {
                        owner.outcome.success_started = true;
                        owner.mark(SyntheticLifecycleMark::first_success_stream_byte);
                    }
                } else {
                    const auto category = error_classification(written.error());
                    if (category != "would_block" && category != "interrupted")
                        owner.terminal_available = false;
                }
                return written;
            } catch (...) {
                // An exceptional adapter may have written before throwing.
                owner.terminal_available = false;
                throw;
            }
        }
    };
    auto startup_failure(PairwiseV2ComposedStartupFailure failure) noexcept -> Stage {
        using F = PairwiseV2ComposedStartupFailure;
        switch (failure) {
        case F::host_offer:
            return Stage::host_offer;
        case F::capacity_offer:
            return Stage::capacity_offer;
        case F::prepared_offer:
            return Stage::prepared_offer;
        case F::request:
            return Stage::request;
        case F::host_ack:
            return Stage::host_ack;
        case F::capacity_ack:
            return Stage::capacity_ack;
        case F::prepared_ack:
            return Stage::prepared_ack;
        case F::pairwise_accepted:
            return Stage::accepted;
        default:
            return stage;
        }
    }
    auto observe_child() noexcept -> bool {
        try {
            auto status = operations.try_wait(operations.context, *child);
            if (!status) {
                cleanup_failure(Kind::reap, Stage::process, SyntheticCleanupSource::exact_reap);
                return false;
            }
            if (*status) {
                if ((*status)->child != *child) {
                    cleanup_failure(Kind::wrong_child, Stage::process,
                                    SyntheticCleanupSource::exact_reap);
                    return false;
                }
                if (!outcome.observed_exit && capture.facts) {
                    capture.facts->observed_exit = SyntheticCapturedExit{(*status)->signaled, (*status)->value};
                    mark(SyntheticLifecycleMark::observed_exit);
                }
                outcome.observed_exit = **status;
                outcome.exit = **status;
                child_stopped = true;
                classify_exit();
            }
            return true;
        } catch (...) {
            cleanup_failure(Kind::callback, Stage::process, SyntheticCleanupSource::exact_reap);
            return false;
        }
    }
    void classify_exit() noexcept {
        if (!outcome.exit || exit_failure_recorded)
            return;
        exit_failure_recorded = outcome.exit->signaled || outcome.exit->value != 0U;
        if (outcome.exit->signaled)
            record(Kind::process_signaled, Stage::process);
        else if (outcome.exit->value != 0U)
            record(Kind::process_nonzero, Stage::process);
    }
    void graceful_cancel() noexcept {
        if (!outcome.cancellation || !outcome.request_complete || forced)
            return;
        if (!send_cancel())
            return;
        if (bound_cancel && !outcome.runner_observed_cancel) {
            const auto reason = static_cast<std::size_t>(*outcome.cancellation);
            if (!transfer(cancel_frames[reason], nullptr, channels.runner_control,
                          SyntheticIo::cancel_read, Kind::cancel_response))
                return;
            outcome.runner_observed_cancel = true;
            mark(SyntheticLifecycleMark::runner_observed_cancel);
            if (terminal_available) {
                terminal_available = false;
                const auto observed_phase = outcome.activated ? 3U : 1U;
                const auto& response = cancelled_frames[reason * phases.size() + observed_phase];
                if (!transfer(response, channels.runner_event, nullptr,
                              SyntheticIo::cancelled_write, Kind::cancel_response)) return;
                if (!transfer(response, nullptr, channels.controller_event,
                              SyntheticIo::cancelled_read, Kind::cancel_response)) return;
                if (capture.facts) capture.facts->cancelled_validated_phase = static_cast<SyntheticCancelledPhase>(observed_phase);
                mark(SyntheticLifecycleMark::cancelled_frame_validated);
                outcome.cancelled_received = true;
            }
            close(SyntheticEndpoint::runner_event, Stage::cleanup);
            if (outcome.cancelled_received) {
                while (!forced) {
                    if (!ready(SyntheticIo::cancelled_read, Stage::eof))
                        continue;
                    try {
                        auto tail = channels.controller_event->read(
                            std::span<std::byte>{receive}.first(1U));
                        if (!tail) {
                            const auto category = error_classification(tail.error());
                            const bool retry =
                                category == "would_block" || category == "interrupted";
                            retire_json(tail.error().context);
                            if (retry)
                                continue;
                            record(Kind::cancel_response, Stage::eof);
                            forced = true;
                        } else if (*tail != 0U) {
                            record(Kind::cancel_response, Stage::eof);
                            forced = true;
                        } else {
                            outcome.clean_eof = true;
                            mark(SyntheticLifecycleMark::clean_eof);
                        }
                        static_cast<void>(point(Stage::eof));
                    } catch (...) {
                        record(Kind::callback, Stage::eof);
                        forced = true;
                    }
                    break;
                }
            }
        }
        while (!child_stopped && !forced) {
            if (!point(Stage::process) || !observe_child() || !point(Stage::process))
                break;
        }
    }
    void retire() noexcept {
        if (state == State::retired || state == State::consumed)
            return;
        mark(SyntheticLifecycleMark::retirement_begun);
        if (!child) {
            if (channels.controller_control) {
                close(SyntheticEndpoint::parent_runner_event, Stage::cleanup);
                close(SyntheticEndpoint::parent_runner_control, Stage::cleanup);
                close_runner();
                close_controller();
            }
            state = State::retired;
            mark(SyntheticLifecycleMark::retirement_completed);
            return;
        }
        graceful_cancel();
        close_runner();
        if (outcome.primary || !child_stopped)
            close_controller();
        if (!child_stopped) {
            outcome.terminate_attempted = true;
            mark(SyntheticLifecycleMark::terminate_attempt);
            try {
                const auto returned = operations.terminate(operations.context, *child);
                if (capture.facts) capture.facts->terminate = returned ? SyntheticCapturedReturn::success : SyntheticCapturedReturn::failed;
                mark(SyntheticLifecycleMark::terminate_return);
                if (!returned)
                    cleanup_failure(Kind::terminate, Stage::process,
                                    SyntheticCleanupSource::graceful_termination);
            } catch (...) {
                if (capture.facts) capture.facts->terminate = SyntheticCapturedReturn::exception;
                mark(SyntheticLifecycleMark::terminate_return);
                cleanup_failure(Kind::callback, Stage::process,
                                SyntheticCleanupSource::graceful_termination);
            }
            stage = Stage::process;
            forced = false;
            const auto now = time();
            if (!now || !begin(Phase::termination, *now))
                forced = true;
            while (!child_stopped && !forced) {
                if (!point(Stage::process, false) || !observe_child() ||
                    !point(Stage::process, false))
                    break;
            }
            if (!child_stopped) {
                outcome.kill_attempted = true;
                mark(SyntheticLifecycleMark::kill_attempt);
                try {
                    const auto returned = operations.kill(operations.context, *child);
                    if (capture.facts) capture.facts->kill = returned ? SyntheticCapturedReturn::success : SyntheticCapturedReturn::failed;
                    mark(SyntheticLifecycleMark::kill_return);
                    if (!returned)
                        cleanup_failure(Kind::kill, Stage::process,
                                        SyntheticCleanupSource::forced_escalation);
                } catch (...) {
                    if (capture.facts) capture.facts->kill = SyntheticCapturedReturn::exception;
                    mark(SyntheticLifecycleMark::kill_return);
                    cleanup_failure(Kind::callback, Stage::process,
                                    SyntheticCleanupSource::forced_escalation);
                }
            }
        }
        if (!reap_attempted) {
            reap_attempted = true;
            static_cast<void>(point(Stage::process));
            mark(SyntheticLifecycleMark::reap_attempt);
            try {
                const auto reaped = operations.reap(operations.context, *child);
                if (capture.facts) capture.facts->reap = !reaped ? SyntheticCapturedReturn::failed :
                    reaped->child != *child ? SyntheticCapturedReturn::wrong_child :
                    (outcome.exit && (outcome.exit->signaled != reaped->signaled || outcome.exit->value != reaped->value)) ?
                    SyntheticCapturedReturn::inconsistent_exit : SyntheticCapturedReturn::success;
                if (capture.facts && reaped)
                    capture.facts->reaped_exit = SyntheticCapturedExit{reaped->signaled, reaped->value};
                mark(SyntheticLifecycleMark::reap_return);
                if (reaped && reaped->child == *child) {
                    if (outcome.exit && (outcome.exit->signaled != reaped->signaled ||
                                         outcome.exit->value != reaped->value))
                        cleanup_failure(Kind::reap, Stage::process,
                                        SyntheticCleanupSource::exact_reap);
                    outcome.exit = *reaped;
                    outcome.exact_reap = true;
                } else
                    cleanup_failure(reaped ? Kind::wrong_child : Kind::reap, Stage::process,
                                    SyntheticCleanupSource::exact_reap);
            } catch (...) {
                if (capture.facts) capture.facts->reap = SyntheticCapturedReturn::exception;
                mark(SyntheticLifecycleMark::reap_return);
                cleanup_failure(Kind::callback, Stage::process, SyntheticCleanupSource::exact_reap);
            }
            static_cast<void>(point(Stage::process));
            if (!outcome.exact_reap) {
                record(Kind::unreapable, Stage::process);
                outcome.fatal_unreapable = true;
                mark(SyntheticLifecycleMark::fatal_unreapable_observed);
                operations.unreapable(operations.context, *child);
            }
        }
        classify_exit();
        close_controller();
        state = State::retired;
        mark(SyntheticLifecycleMark::retirement_completed);
    }
    auto activate_evidence() noexcept -> bool {
        outcome.activated = true;
        if (capture.facts) capture.facts->activation_intended = true;
        if (!point(Stage::backend) || outcome.cancellation)
            return false;
        mark(SyntheticLifecycleMark::backend_entered);
        const auto produced = runner->produce(backend);
        if (!produced) {
            record(Kind::backend, Stage::backend, produced.error());
            static_cast<void>(point(Stage::backend));
            return false;
        }
        mark(SyntheticLifecycleMark::backend_returned_evidence);
        if (!point(Stage::backend) || outcome.cancellation)
            return false;
        if (!point(Stage::encoding) || outcome.cancellation)
            return false;
        const auto encoded = runner->encode();
        if (!encoded) {
            record(Kind::encoding, Stage::encoding, encoded.error());
            static_cast<void>(point(Stage::encoding));
            return false;
        }
        mark(SyntheticLifecycleMark::canonical_encoded);
        if (!point(Stage::encoding) || outcome.cancellation)
            return false;
        const auto receiving = controller->start();
        if (!receiving) {
            record(Kind::transport, Stage::manifest, receiving.error());
            return false;
        }
        return point(Stage::manifest) && !outcome.cancellation;
    }
    void execute() noexcept {
        if (!activate_evidence()) {
            retire();
            return;
        }
        bool sent{}, eof{};
        EvidenceWriter output{*this};
        while (!eof && !outcome.primary) {
            if (!point(outcome.completion_received ? Stage::eof : Stage::chunks) ||
                outcome.cancellation)
                break;
            const auto sending_stage = transport_stage(runner->next_stage());
            if (!sent && ready(SyntheticIo::event_write, sending_stage) && !outcome.cancellation) {
                const auto progress = runner->write_step(output, PreparedTransportSignal::none);
                if (!progress) {
                    record(Kind::transport, sending_stage, progress.error());
                    break;
                }
                sent = *progress == Progress::complete;
                if (sent) {
                    mark(SyntheticLifecycleMark::sender_completed);
                    close(SyntheticEndpoint::runner_event, Stage::cleanup);
                }
                if (!point(sending_stage) || outcome.cancellation)
                    break;
            }
            const auto receiving_stage = transport_stage(controller->next_stage());
            if (ready(SyntheticIo::event_read, receiving_stage) && !outcome.cancellation) {
                const auto progress = controller->read_step(*channels.controller_event,
                                                            PreparedTransportSignal::none);
                if (!progress) {
                    record(Kind::transport, receiving_stage, progress.error());
                    break;
                }
                if (controller->completion_received()) first_mark(SyntheticLifecycleMark::completion_received);
                if (*progress == Progress::complete) first_mark(SyntheticLifecycleMark::clean_eof);
                if (!point(receiving_stage) || outcome.cancellation)
                    break;
                if (!outcome.completion_received && controller->completion_received()) {
                    outcome.completion_received = true;
                    const auto now = time();
                    if (!now || !begin(Phase::completion, *now))
                        break;
                }
                eof = *progress == Progress::complete;
            }
        }
        if (outcome.cancellation) {
            static_cast<void>(
                runner->write_step(*channels.runner_event, PreparedTransportSignal::cancelled));
            static_cast<void>(controller->read_step(*channels.controller_event,
                                                    PreparedTransportSignal::cancelled));
        }
        if (!outcome.primary && eof) {
            outcome.clean_eof = true;
            first_mark(SyntheticLifecycleMark::clean_eof);
            if (point(Stage::reconstruction) && !outcome.cancellation) {
                const auto reconstructed = controller->reconstruct();
                if (!reconstructed)
                    record(Kind::reconstruction, Stage::reconstruction, reconstructed.error());
                else mark(SyntheticLifecycleMark::typed_reconstruction_completed);
                if (point(Stage::reconstruction) && !outcome.primary && point(Stage::results) &&
                    !outcome.primary) {
                    auto result = controller->project();
                    if (!result)
                        record(Kind::results, Stage::results, result.error());
                    else {
                        outcome.result.emplace(std::move(*result));
                        mark(SyntheticLifecycleMark::results_projected);
                    }
                    static_cast<void>(point(Stage::results));
                }
            }
        }
        while (!outcome.primary && !child_stopped) {
            if (!point(Stage::process) || !observe_child() || !point(Stage::process))
                break;
        }
        retire();
    }
};

auto synthetic_capture_usage(const wire_v1::detail::AuthoredCapacityOffer& offer,
                             std::string_view request) noexcept
    -> std::expected<SyntheticCaptureUsage, SyntheticCaptureFailure> {
    SyntheticCaptureCopyStaging staging{};
    staging.original = &offer;
    staging.request = request;
    const auto add = [&](std::uint64_t& total, std::uint64_t count) noexcept {
        if (count > std::numeric_limits<std::uint64_t>::max() - total) return false;
        total += count;
        return true;
    };
    const auto lane = [&](std::uint64_t count, std::uint64_t width) noexcept {
        if (width != 0U && count > std::numeric_limits<std::uint64_t>::max() / width)
            return false;
        return add(staging.elements, count) && add(staging.capacity_backing, count * width);
    };
    const auto& parameters = offer.capacity.projected().capacity.prerequisite.parameters;
    using namespace wire_v1;
    bool valid = add(staging.text, request.size()) &&
                 add(staging.text, offer.canonical_body.size()) &&
                 add(staging.text, offer.capacity.session_id().text().size()) &&
                 add(staging.text, offer.capacity.family().size()) &&
                 lane(parameters.present_cpu_ids.size(), sizeof(CpuId)) &&
                 lane(parameters.domain_cpu_ids.size(), sizeof(std::vector<CpuId>)) &&
                 lane(parameters.first_total_pages.size(), sizeof(PageCount)) &&
                 lane(parameters.second_total_pages.size(), sizeof(PageCount)) &&
                 lane(parameters.first_target_counts.size(), sizeof(TargetCount)) &&
                 lane(parameters.second_target_counts.size(), sizeof(TargetCount));
    for (const auto& domain : parameters.domain_cpu_ids)
        valid = valid && lane(domain.size(), sizeof(CpuId));
    if (!valid) return std::unexpected{SyntheticCaptureFailure::overflow};
    staging.usage.request_bytes = request.size();
    staging.usage.offer_bytes = offer.canonical_body.size();
    staging.usage.session_bytes = offer.capacity.session_id().text().size();
    staging.usage.family_bytes = offer.capacity.family().size();
    staging.usage.text_bytes = staging.text;
    staging.usage.capacity_elements = staging.elements;
    staging.usage.retained_bytes = sizeof(SyntheticLifecycleFacts) + synthetic_capture_inline_bytes;
    valid = add(staging.usage.retained_bytes, staging.text) &&
            add(staging.usage.retained_bytes, staging.capacity_backing);
    staging.usage.peak_bytes = staging.usage.retained_bytes;
    // The source original remains owned by composed startup throughout this
    // copy. Include its complete owning header and dynamic lanes separately.
    valid = valid && add(staging.usage.peak_bytes, sizeof(SyntheticCapturePreparationInventory)) &&
            add(staging.usage.peak_bytes, sizeof(wire_v1::detail::AuthoredCapacityOffer)) &&
            add(staging.usage.peak_bytes, staging.capacity_backing) &&
            add(staging.usage.peak_bytes, offer.canonical_body.size()) &&
            add(staging.usage.peak_bytes, offer.capacity.session_id().text().size()) &&
            add(staging.usage.peak_bytes, offer.capacity.family().size());
    if (!valid) return std::unexpected{SyntheticCaptureFailure::overflow};
    return staging.usage;
}

PairwiseV2SyntheticLifecycle::PairwiseV2SyntheticLifecycle(std::unique_ptr<Storage> state) noexcept
    : storage_{std::move(state)} {}
PairwiseV2SyntheticLifecycle::PairwiseV2SyntheticLifecycle(
    PairwiseV2SyntheticLifecycle&&) noexcept = default;
PairwiseV2SyntheticLifecycle::~PairwiseV2SyntheticLifecycle() noexcept {
    if (storage_ && storage_->state != Storage::State::retired &&
        storage_->state != Storage::State::consumed) {
        storage_->record(Kind::invalid_state, Stage::cleanup);
        storage_->forced = true;
        storage_->retire();
    }
}
auto PairwiseV2SyntheticLifecycle::prepare(
    wire_v1::detail::StartupSessionId session, const PairwiseHostCapacityRequestTranscript& request,
    const RunnerContext& controller_context, RunnerContext runner_context,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations controller_authority,
    PairwiseMonitoringHostCapacityStartupAccess::LowerOperations runner_authority,
    SyntheticLifecycleConfig config, SyntheticProcessOperations operations,
    SyntheticBackendPreparation fixture, const SyntheticCaptureLimits* capture) noexcept
    -> std::expected<PairwiseV2SyntheticLifecycle, SyntheticFailure> {
    const auto fail = [](Kind kind) {
        return std::unexpected{
            SyntheticFailure{kind, Stage::preparation, Phase::startup, std::nullopt}};
    };
    if (config.startup < 0 || config.execution < 0 || config.cancellation < 0 ||
        config.completion < 0 || config.termination < 0 || !operations.now ||
        !operations.channels || !operations.spawn || !operations.close || !operations.ready ||
        !operations.try_wait || !operations.terminate || !operations.kill || !operations.reap ||
        !operations.unreapable || !fixture.prepare)
        return fail(Kind::invalid_config);
    try {
        std::optional<core::Sha256Digest> host_offer_digest;
        auto controller = PairwiseV2ComposedStartupController::prepare(
            session, request, controller_context, controller_authority,
            capture ? &host_offer_digest : nullptr);
        if (!controller)
            return fail(Kind::preparation);
        SyntheticCaptureCopyStaging copied{};
        if (capture) {
            copied.original = controller->prepared_capacity_offer();
            if (!copied.original || !host_offer_digest) return fail(Kind::preparation);
            copied.counted = synthetic_capture_usage(*copied.original, request.canonical_body());
            if (!copied.counted || copied.counted->request_bytes > capture->request_bytes ||
                copied.counted->offer_bytes > capture->offer_bytes ||
                copied.counted->session_bytes > capture->session_bytes ||
                copied.counted->family_bytes > capture->family_bytes ||
                copied.counted->text_bytes > capture->text_bytes ||
                copied.counted->capacity_elements > capture->capacity_elements ||
                copied.counted->retained_bytes > capture->retained_bytes ||
                copied.counted->peak_bytes > capture->peak_bytes)
                return fail(Kind::preparation);
            copied.copied = std::make_unique<SyntheticLifecycleFacts>(
                PairwiseV2PairIdentity{request.first_request_id(), request.second_request_id()},
                *host_offer_digest);
            copied.copied->request_body = request.canonical_body();
            copied.copied->capacity_offer.emplace(*copied.original);
            copied.copied->config = config;
            copied.copied->usage = *copied.counted;
        }
        auto state = std::make_unique<Storage>(
            std::move(*controller), std::move(session),
            PairwiseV2PairIdentity{request.first_request_id(), request.second_request_id()},
            std::move(runner_context), runner_authority, config, operations, fixture);
        state->capture.facts = std::move(copied.copied);
        return PairwiseV2SyntheticLifecycle{std::move(state)};
    } catch (...) {
        return fail(Kind::preparation);
    }
}
auto PairwiseV2SyntheticLifecycle::start() noexcept -> std::expected<void, SyntheticFailure> {
    const SyntheticFailure invalid{Kind::invalid_state, Stage::preparation, Phase::startup,
                                   std::nullopt};
    if (!storage_)
        return std::unexpected{invalid};
    auto& state = *storage_;
    if (state.state != Storage::State::prepared) {
        if (state.state != Storage::State::retired && state.state != Storage::State::consumed) {
            state.record(Kind::invalid_state, Stage::preparation);
            state.forced = true;
            state.retire();
        }
        return std::unexpected{invalid};
    }
    state.state = Storage::State::starting;
    const auto failure = [&]() -> std::expected<void, SyntheticFailure> {
        if (!state.outcome.primary)
            state.record(Kind::protocol, state.stage);
        state.retire();
        return std::unexpected{*state.outcome.primary};
    };
    try {
        if (!state.point(Stage::preparation))
            return failure();
        if (!state.point(Stage::channels))
            return failure();
        const auto channels = state.operations.channels(state.operations.context);
        if (!channels || !channels->controller_control || !channels->runner_control ||
            !channels->runner_event || !channels->controller_event) {
            state.record(Kind::channels, Stage::channels);
            return failure();
        }
        state.channels = *channels;
        state.mark(SyntheticLifecycleMark::channels_ready);
        if (!state.point(Stage::channels))
            return failure();
        if (!state.point(Stage::spawn))
            return failure();
        const auto child = state.operations.spawn(state.operations.context);
        if (!child || child->token == 0U) {
            state.record(Kind::spawn, Stage::spawn);
            return failure();
        }
        state.child = *child;
        state.mark(SyntheticLifecycleMark::child_created);
        state.stage = Stage::spawn;
        const auto now = state.time();
        if (!now || !state.begin(Phase::startup, *now))
            return failure();
        state.close(SyntheticEndpoint::parent_runner_event, Stage::parent_close);
        state.close(SyntheticEndpoint::parent_runner_control, Stage::parent_close);
        if (state.outcome.primary || !state.point(Stage::parent_close))
            return failure();
        Storage::StartupIo control_out{state, SyntheticIo::control_write,
                                       state.channels.controller_control, nullptr,
                                       state.control_position};
        const auto sent = state.startup.write_control(control_out);
        if (!sent) {
            if (!state.outcome.primary)
                state.record(Kind::protocol, state.startup_failure(sent.error()));
            return failure();
        }
        if (!state.point(Stage::request) || !state.send_cancel())
            return failure();
        Storage::StartupIo control_in{state, SyntheticIo::control_read, nullptr,
                                      state.channels.runner_control, state.runner_control_position};
        auto runner = PairwiseV2ComposedStartupRunner::admit(
            control_in, state.session, state.pair.first, std::move(state.runner_context),
            state.runner_authority);
        if (!runner) {
            if (!state.outcome.primary)
                state.record(Kind::protocol, state.startup_failure(runner.error()));
            return failure();
        }
        if (!state.point(Stage::prepared_offer))
            return failure();
        if (const auto* admission = runner->admitted_before_ack()) {
            const auto prepared = state.fixture.prepare(state.fixture.context, *admission);
            if (!prepared || !prepared->produce) {
                state.record(Kind::preparation, Stage::backend);
                return failure();
            }
            state.backend = *prepared;
        }
        if (!state.point(Stage::prepared_ack))
            return failure();
        Storage::StartupIo event_out{state, SyntheticIo::event_write, state.channels.runner_event,
                                     nullptr, state.event_position};
        auto runner_role = runner->write_acknowledgements(event_out);
        if (!runner_role) {
            if (!state.outcome.primary)
                state.record(Kind::protocol, state.startup_failure(runner_role.error()));
            return failure();
        }
        Storage::StartupIo event_in{state, SyntheticIo::event_read, nullptr,
                                    state.channels.controller_event,
                                    state.controller_event_position};
        auto controller_role = state.startup.read_acknowledgements(event_in);
        if (!controller_role) {
            if (!state.outcome.primary)
                state.record(Kind::protocol, state.startup_failure(controller_role.error()));
            return failure();
        }
        if (state.capture.facts) {
            const auto* sealed = std::get_if<PairwiseV2ControllerResources>(&*controller_role);
            const auto* other = std::get_if<PairwiseV2RunnerResources>(&*runner_role);
            if (sealed && other) {
                const auto& id = sealed->identity();
                if (id != other->identity() ||
                    id.startup_session != state.capture.facts->capacity_offer->capacity.session_id()) {
                    state.capture.facts->capture_failure = SyntheticCaptureFailure::identity;
                } else {
                    state.capture.facts->identity = SyntheticCapturedIdentity{
                        id.pair, id.request_body_sha256, id.capacity_offer_body_sha256,
                        id.capacity_body_sha256, id.capacity_ack_body_sha256,
                        id.prepared_offer_body_sha256, id.prepared_resource_sha256,
                        id.prepared_ack_body_sha256, id.wire_registry_sha256,
                        id.source_manifest_sha256};
                    state.mark(SyntheticLifecycleMark::acknowledgments_validated);
                }
            }
        }
        if (!state.point(Stage::accepted))
            return failure();
        auto* controller = std::get_if<PairwiseV2ControllerResources>(&*controller_role);
        auto* producer = std::get_if<PairwiseV2RunnerResources>(&*runner_role);
        if (!controller || !producer) {
            state.record(Kind::protocol, Stage::host_ack);
            return failure();
        }
        state.outcome.accepted = true;
        if (!PairwiseV2SyntheticProtocolAccess::bind(*controller, *producer)) {
            state.record(Kind::protocol, Stage::accepted);
            return failure();
        }
        state.bound_cancel = true;
        if (!state.outcome.cancellation) {
            const auto accepted_time = state.time();
            if (!accepted_time || !state.begin(Phase::execution, *accepted_time))
                return failure();
        }
        auto active_runner = PairwiseV2ActiveEvidenceRunner::bind(std::move(*producer));
        auto active_controller = PairwiseV2ActiveEvidenceController::bind(std::move(*controller));
        if (!active_runner || !active_controller) {
            state.record(Kind::protocol, Stage::accepted);
            return failure();
        }
        state.runner.emplace(std::move(*active_runner));
        state.controller.emplace(std::move(*active_controller));
        state.mark(SyntheticLifecycleMark::active_roles_bound);
        if (state.outcome.cancellation)
            return failure();
        if (!state.point(Stage::accepted))
            return failure();
        state.state = Storage::State::ready;
        return {};
    } catch (...) {
        state.record(Kind::callback, state.stage);
        return failure();
    }
}
auto PairwiseV2SyntheticLifecycle::finish() noexcept -> SyntheticLifecycleOutcome {
    if (!storage_ || storage_->state == Storage::State::consumed) {
        SyntheticLifecycleOutcome result;
        result.primary =
            SyntheticFailure{Kind::invalid_state, Stage::preparation, Phase::startup, std::nullopt};
        return result;
    }
    auto& state = *storage_;
    if (state.state == Storage::State::ready)
        state.execute();
    else if (state.state != Storage::State::retired) {
        state.record(Kind::invalid_state, Stage::preparation);
        state.forced = true;
        state.retire();
    }
    state.state = Storage::State::consumed;
    if (state.capture.facts) state.capture.facts->outcome_delivered = true;
    return std::move(state.outcome);
}

auto PairwiseV2SyntheticLifecycle::capture_retired() noexcept
    -> std::expected<SyntheticLifecycleView, SyntheticCaptureFailure> {
    if (!storage_ || !storage_->capture.facts || storage_->capture.taken ||
        (storage_->state != Storage::State::retired && storage_->state != Storage::State::consumed))
        return std::unexpected{SyntheticCaptureFailure::invalid_state};
    storage_->capture.taken = true;
    if (storage_->capture.facts->capture_failure)
        return std::unexpected{*storage_->capture.facts->capture_failure};
    return SyntheticLifecycleView{storage_->capture.facts.get(),
        storage_->runner ? &storage_->runner->admitted_for_retired_capture() : nullptr,
        storage_->controller ? storage_->controller->validated_bytes() : std::span<const std::byte>{}};
}

} // namespace epycbench::runtime::detail
