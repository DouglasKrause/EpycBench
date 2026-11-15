#include "epycbench/runtime/controller_lifecycle.hpp"

#include "runtime/controller_ipc_adapters.hpp"
#include "runtime/controller_lifecycle_access.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::runtime {
namespace detail {
namespace {

struct Evidence {
    std::optional<core::Error> primary;
    std::vector<core::Error> cleanup_errors;
    bool accepted{};
    std::optional<ipc::Envelope> terminal;
    bool eof{};
    std::optional<platform_linux::ProcessExitStatus> status;
    std::optional<ipc::CancelReason> cancellation;
    bool escalated{};
};

[[nodiscard]] auto controller_error(std::string_view code, std::string_view operation,
                                    std::string_view classification, std::string_view phase = {},
                                    std::optional<core::Error> underlying = std::nullopt)
    -> core::Error {
    nlohmann::json context{{"classification", classification}};
    if (!phase.empty())
        context["controller_phase"] = phase;
    if (underlying)
        context["underlying_code"] = underlying->code.value();
    return core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{std::string{code}},
        .message = "Controller lifecycle operation failed",
        .operation = std::string{operation},
        .os_error = underlying ? underlying->os_error : std::nullopt,
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

void record(Evidence& evidence, core::Error error) {
    if (!evidence.primary)
        evidence.primary.emplace(std::move(error));
}

void record_cleanup(Evidence& evidence, core::Error error) {
    evidence.cleanup_errors.push_back(std::move(error));
}

[[nodiscard]] auto failure(Evidence evidence) -> ControllerResult {
    return std::unexpected{ControllerFailure{
        .primary = std::move(*evidence.primary),
        .request_accepted = evidence.accepted,
        .terminal_event = std::move(evidence.terminal),
        .event_eof_observed = evidence.eof,
        .process_status = std::move(evidence.status),
        .cancellation_reason = evidence.cancellation,
    }};
}

[[nodiscard]] auto cancel_token(ipc::CancelReason reason) -> std::string_view {
    switch (reason) {
    case ipc::CancelReason::user_interrupt:
        return "user_interrupt";
    case ipc::CancelReason::controller_shutdown:
        return "controller_shutdown";
    case ipc::CancelReason::controller_timeout:
        return "controller_timeout";
    }
    return "controller_timeout";
}

[[nodiscard]] auto external_reason(ControllerCancellation cancellation)
    -> std::optional<ipc::CancelReason> {
    switch (cancellation) {
    case ControllerCancellation::none:
        return std::nullopt;
    case ControllerCancellation::user_interrupt:
        return ipc::CancelReason::user_interrupt;
    case ControllerCancellation::controller_shutdown:
        return ipc::CancelReason::controller_shutdown;
    }
    return std::nullopt;
}

[[nodiscard]] auto observe(CancellationObserver observer) -> std::optional<ipc::CancelReason> {
    if (observer.observe == nullptr)
        return std::nullopt;
    return external_reason(observer.observe(observer.context));
}

[[nodiscard]] auto validate_config(const ControllerLifecycleConfig& config) -> bool {
    return config.startup_timeout.count() >= 0 && config.execution_timeout.count() >= 0 &&
           config.cancellation_grace.count() >= 0 && config.completion_grace.count() >= 0 &&
           config.termination_grace.count() >= 0 && config.observation_quantum.count() > 0;
}

[[nodiscard]] auto normal_zero(const platform_linux::ProcessExitStatus& status) -> bool {
    const auto* exited = std::get_if<platform_linux::ProcessExited>(&status);
    return exited != nullptr && exited->code == 0;
}

[[nodiscard]] auto wrap_lower(std::string_view code, std::string_view operation,
                              std::string_view classification, std::string_view phase,
                              core::Error error) -> core::Error {
    return controller_error(code, operation, classification, phase, std::move(error));
}

[[nodiscard]] auto create_placement(const ControllerRequest& request)
    -> std::expected<ipc::Envelope, core::Error> {
    auto payload =
        ipc::validate_payload(ipc::MessageType::placement_request, request.placement.wire());
    if (!payload)
        return std::unexpected{std::move(payload.error())};
    const auto profile = request.placement.transport_version()==ipc::PlacementRequestTransportVersion::v4
                             ? "epycbench.placement-request/v4"
                         : request.placement.transport_version()==ipc::PlacementRequestTransportVersion::v3
                             ? "epycbench.placement-request/v3"
                             : "epycbench.placement-request/v2";
    return ipc::Envelope::create(ipc::MessageType::placement_request, request.request_id,
                                 ipc::SequenceNumber::create(0), request.placement.wire(), profile);
}

[[nodiscard]] auto create_cancel(ipc::RequestId request_id, ipc::CancelReason reason)
    -> std::expected<ipc::Envelope, core::Error> {
    nlohmann::ordered_json payload{{"reason", cancel_token(reason)}};
    auto valid = ipc::validate_payload(ipc::MessageType::cancel_request, payload);
    if (!valid)
        return std::unexpected{std::move(valid.error())};
    return ipc::Envelope::create(ipc::MessageType::cancel_request, request_id,
                                 ipc::SequenceNumber::create(1), std::move(payload));
}

[[noreturn]] void production_fatal(void*) noexcept { std::terminate(); }

class ChildEmergencyGuard {
  public:
    ChildEmergencyGuard(platform_linux::ChildProcess& child,
                        platform_linux::LocalIpcEndpoint& control,
                        ControllerFatalInvariant fatal) noexcept
        : child_{&child}, control_{&control}, fatal_{fatal} {}
    ChildEmergencyGuard(const ChildEmergencyGuard&) = delete;
    auto operator=(const ChildEmergencyGuard&) -> ChildEmergencyGuard& = delete;
    ~ChildEmergencyGuard() noexcept {
        if (!child_->active())
            return;
        try {
            static_cast<void>(control_->close());
            static_cast<void>(child_->send_signal(platform_linux::ProcessSignal::terminate));
            if (child_->active()) {
                static_cast<void>(child_->send_signal(platform_linux::ProcessSignal::kill));
            }
            auto waited = child_->wait();
            if (!waited)
                fatal_.unreapable(fatal_.context);
        } catch (...) {
            fatal_.unreapable(fatal_.context);
        }
    }

  private:
    platform_linux::ChildProcess* child_;
    platform_linux::LocalIpcEndpoint* control_;
    ControllerFatalInvariant fatal_;
};

auto production_channels(void*) -> std::expected<platform_linux::LocalIpcChannels, core::Error> {
    return platform_linux::create_local_ipc_channels();
}

auto production_spawn(void*, const platform_linux::ChildSpawnRequest& request,
                      const platform_linux::RunnerIpcEndpoints& endpoints)
    -> std::expected<platform_linux::ChildProcess, core::Error> {
    return platform_linux::spawn_child(request, endpoints);
}

void close_endpoint(platform_linux::LocalIpcEndpoint& endpoint, Evidence& evidence,
                    std::string_view classification, std::string_view phase) {
    auto closed = endpoint.close();
    if (!closed) {
        record_cleanup(evidence,
                       wrap_lower("runtime.controller.cleanup_failed", "controller.close_endpoint",
                                  classification, phase, std::move(closed.error())));
    }
}

[[nodiscard]] auto read_event(platform_linux::LocalIpcEndpoint& endpoint, ControllerClock clock,
                              const ControllerTimePoint& deadline,
                              std::string_view timeout_classification)
    -> std::expected<ipc::FrameReadResult, core::Error> {
    DeadlineEventReader reader{endpoint, clock, deadline, timeout_classification};
    return ipc::read_frame(reader);
}

void exact_reap(platform_linux::ChildProcess& child, Evidence& evidence,
                ControllerFatalInvariant fatal) {
    if (!child.active())
        return;
    try {
        auto waited = child.wait();
        if (!waited)
            fatal.unreapable(fatal.context);
        evidence.status = std::move(*waited);
    } catch (...) {
        fatal.unreapable(fatal.context);
    }
}

void force_cleanup(platform_linux::ChildProcess& child, platform_linux::LocalIpcEndpoint& control,
                   platform_linux::LocalIpcEndpoint& events,
                   const ControllerLifecycleConfig& config, ControllerClock clock,
                   ControllerFatalInvariant fatal, ipc::PlacementEventState& event_state,
                   Evidence& evidence) {
    close_endpoint(control, evidence, "control_close_failed", "termination");
    if (child.active()) {
        evidence.escalated = true;
        auto term = child.send_signal(platform_linux::ProcessSignal::terminate);
        if (!term) {
            record(evidence,
                   wrap_lower("runtime.controller.termination_failed", "controller.send_sigterm",
                              "sigterm_failed", "termination", std::move(term.error())));
        }

        auto deadline = checked_deadline(clock, config.termination_grace);
        if (!deadline) {
            record(evidence, std::move(deadline.error()));
        } else {
            while (child.active() && clock.now(clock.context) < *deadline) {
                auto status = child.try_wait();
                if (status && *status) {
                    evidence.status = std::move(**status);
                    break;
                }
                if (!status &&
                    status.error().context.value("classification", "") != "interrupted") {
                    break;
                }
                auto frame = read_event(events, clock, *deadline, "termination_timeout");
                if (!frame)
                    break;
                if (auto* envelope = std::get_if<ipc::Envelope>(&*frame)) {
                    if (event_state.accept(*envelope)) {
                        if (envelope->type() == ipc::MessageType::request_accepted) {
                            evidence.accepted = true;
                        } else {
                            evidence.terminal = std::move(*envelope);
                        }
                    }
                } else {
                    evidence.eof = true;
                    break;
                }
            }
        }
    }
    if (child.active()) {
        auto killed = child.send_signal(platform_linux::ProcessSignal::kill);
        if (!killed) {
            record(evidence,
                   wrap_lower("runtime.controller.termination_failed", "controller.send_sigkill",
                              "sigkill_failed", "termination", std::move(killed.error())));
        }
    }
    exact_reap(child, evidence, fatal);
    close_endpoint(events, evidence, "event_close_failed", "cleanup");
}

} // namespace

auto production_controller_dependencies() noexcept -> ControllerDependencies {
    return ControllerDependencies{
        .channels = ControllerChannelFactory{nullptr, production_channels},
        .spawner = ControllerProcessSpawner{nullptr, production_spawn},
        .clock = production_controller_clock(),
        .fatal = ControllerFatalInvariant{nullptr, production_fatal},
    };
}

auto run_controller_request_with(const ControllerRequest& request,
                                 CancellationObserver cancellation,
                                 ControllerDependencies dependencies) -> ControllerResult {
    Evidence evidence;
    try {
        if (request.executable.empty() || !validate_config(request.lifecycle) ||
            dependencies.channels.create == nullptr || dependencies.spawner.spawn == nullptr ||
            dependencies.clock.now == nullptr || dependencies.fatal.unreapable == nullptr ||
            (cancellation.observe == nullptr && cancellation.context != nullptr)) {
            record(evidence, controller_error("runtime.controller.invalid_argument",
                                              "controller.validate", "invalid_lifecycle_config"));
            return failure(std::move(evidence));
        }

        auto placement = create_placement(request);
        if (!placement) {
            record(evidence, wrap_lower("runtime.controller.invalid_argument",
                                        "controller.validate_placement", "invalid_request",
                                        "validation", std::move(placement.error())));
            return failure(std::move(evidence));
        }
        ipc::RunnerControlState control_state;
        auto control_valid = control_state.accept(*placement);
        if (!control_valid) {
            record(evidence,
                   wrap_lower("runtime.controller.invalid_argument", "controller.validate_control",
                              "invalid_request", "validation", std::move(control_valid.error())));
            return failure(std::move(evidence));
        }
        if (auto reason = observe(cancellation)) {
            evidence.cancellation = reason;
            record(evidence, controller_error("runtime.controller.cancelled",
                                              "controller.observe_cancellation",
                                              "cancelled_before_start", "validation"));
            return failure(std::move(evidence));
        }

        auto channels_result = dependencies.channels.create(dependencies.channels.context);
        if (!channels_result) {
            record(evidence, wrap_lower("runtime.controller.transport_setup_failed",
                                        "controller.create_channels", "channel_creation_failed",
                                        "startup", std::move(channels_result.error())));
            return failure(std::move(evidence));
        }
        auto channels = std::move(*channels_result);
        platform_linux::ChildSpawnRequest spawn_request{
            .executable = request.executable,
            .arguments = {request.executable, "__runner"},
        };
        auto child_result = dependencies.spawner.spawn(dependencies.spawner.context, spawn_request,
                                                       channels.runner);
        if (!child_result) {
            record(evidence,
                   wrap_lower("runtime.controller.spawn_failed", "controller.spawn", "spawn_failed",
                              "startup", std::move(child_result.error())));
            close_endpoint(channels.runner.event_writer, evidence, "runner_endpoint_close_failed",
                           "startup");
            close_endpoint(channels.runner.control_reader, evidence, "runner_endpoint_close_failed",
                           "startup");
            close_endpoint(channels.controller.control_writer, evidence, "control_close_failed",
                           "startup");
            close_endpoint(channels.controller.event_reader, evidence, "event_close_failed",
                           "startup");
            return failure(std::move(evidence));
        }
        auto child = std::move(*child_result);
        ChildEmergencyGuard emergency_guard{child, channels.controller.control_writer,
                                            dependencies.fatal};

        auto startup_deadline =
            checked_deadline(dependencies.clock, request.lifecycle.startup_timeout);
        if (!startup_deadline)
            record(evidence, std::move(startup_deadline.error()));
        close_endpoint(channels.runner.event_writer, evidence, "runner_endpoint_close_failed",
                       "startup");
        close_endpoint(channels.runner.control_reader, evidence, "runner_endpoint_close_failed",
                       "startup");

        ipc::PlacementEventState event_state{request.request_id, request.placement};
        bool placement_complete = false;
        bool cancel_attempted = false;
        std::optional<ControllerTimePoint> active_deadline;
        if (startup_deadline)
            active_deadline = *startup_deadline;

        if (!evidence.primary && active_deadline) {
            DeadlineControlWriter writer{channels.controller.control_writer, dependencies.clock,
                                         *active_deadline, "acceptance_timeout"};
            auto written = ipc::write_frame(writer, *placement);
            placement_complete = written.has_value();
            if (!written) {
                const auto classification = writer.progress() > 0
                                                ? "partial_control_frame"
                                                : "placement_request_write_failed";
                record(evidence, wrap_lower("runtime.controller.request_write_failed",
                                            "controller.write_placement", classification, "startup",
                                            std::move(written.error())));
            }
        }

        while (!evidence.primary && !evidence.terminal) {
            if (!evidence.cancellation)
                evidence.cancellation = observe(cancellation);
            if (evidence.cancellation && placement_complete && !cancel_attempted) {
                cancel_attempted = true;
                auto cancellation_deadline =
                    checked_deadline(dependencies.clock, request.lifecycle.cancellation_grace);
                if (cancellation_deadline)
                    active_deadline = *cancellation_deadline;
                else
                    active_deadline.reset();
                auto cancel = create_cancel(request.request_id, *evidence.cancellation);
                if (!cancel || !active_deadline) {
                    record(evidence, controller_error("runtime.controller.cancel_failed",
                                                      "controller.create_cancel",
                                                      "cancel_write_failed", "cancellation"));
                    break;
                }
                auto accepted = control_state.accept(*cancel);
                if (!accepted) {
                    record(evidence, wrap_lower("runtime.controller.cancel_failed",
                                                "controller.validate_cancel", "cancel_write_failed",
                                                "cancellation", std::move(accepted.error())));
                    break;
                }
                DeadlineControlWriter writer{channels.controller.control_writer, dependencies.clock,
                                             *active_deadline, "cancellation_grace_expired"};
                auto sent = ipc::write_frame(writer, *cancel);
                if (!sent) {
                    record(evidence,
                           wrap_lower("runtime.controller.cancel_failed", "controller.write_cancel",
                                      writer.progress() > 0 ? "partial_control_frame"
                                                            : "cancel_write_failed",
                                      "cancellation", std::move(sent.error())));
                    break;
                }
            }

            auto status = child.try_wait();
            if (status && *status)
                evidence.status = std::move(**status);
            if (!status && status.error().context.value("classification", "") != "interrupted") {
                record(evidence, wrap_lower("runtime.controller.wait_failed", "controller.try_wait",
                                            "runner_exited_early",
                                            evidence.accepted ? "execution" : "startup",
                                            std::move(status.error())));
                break;
            }

            if (!active_deadline)
                break;
            auto observation_deadline =
                checked_deadline(dependencies.clock, request.lifecycle.observation_quantum);
            if (!observation_deadline) {
                record(evidence, std::move(observation_deadline.error()));
                break;
            }
            if (*active_deadline < *observation_deadline) {
                observation_deadline = *active_deadline;
            }
            auto frame = read_event(channels.controller.event_reader, dependencies.clock,
                                    *observation_deadline,
                                    evidence.cancellation ? "cancellation_grace_expired"
                                    : evidence.accepted   ? "execution_timeout"
                                                          : "acceptance_timeout");
            if (!frame) {
                if (frame.error().code.value() == "runtime.controller.timeout" &&
                    dependencies.clock.now(dependencies.clock.context) < *active_deadline) {
                    continue;
                }
                if (frame.error().code.value() == "runtime.controller.timeout" &&
                    !evidence.cancellation && placement_complete) {
                    evidence.cancellation = ipc::CancelReason::controller_timeout;
                    continue;
                }
                // Own the classification before moving the Error: function
                // argument evaluation must not inspect an already moved value.
                const auto classification =
                    frame.error().code.value() == "runtime.controller.timeout"
                        ? frame.error().context.value("classification", "execution_timeout")
                        : std::string{"event_read_failed"};
                record(evidence,
                       wrap_lower("runtime.controller.event_read_failed", "controller.read_event",
                                  classification, evidence.accepted ? "execution" : "startup",
                                  std::move(frame.error())));
                break;
            }
            if (std::holds_alternative<ipc::EndOfStream>(*frame)) {
                record(evidence, controller_error("runtime.controller.protocol_failed",
                                                  "controller.event_eof", "unexpected_eof",
                                                  evidence.accepted ? "execution" : "startup"));
                break;
            }
            auto envelope = std::get<ipc::Envelope>(std::move(*frame));
            auto valid = event_state.accept(envelope);
            if (!valid) {
                record(evidence,
                       wrap_lower("runtime.controller.protocol_failed", "controller.validate_event",
                                  "unexpected_event", evidence.accepted ? "execution" : "startup",
                                  std::move(valid.error())));
                break;
            }
            if (envelope.type() == ipc::MessageType::request_accepted) {
                evidence.accepted = true;
                if (!evidence.cancellation) {
                    auto execution_deadline =
                        checked_deadline(dependencies.clock, request.lifecycle.execution_timeout);
                    if (execution_deadline)
                        active_deadline = *execution_deadline;
                    else {
                        active_deadline.reset();
                        record(evidence, std::move(execution_deadline.error()));
                    }
                }
            } else {
                evidence.terminal = std::move(envelope);
            }
        }

        if (!evidence.primary && evidence.terminal) {
            auto control_ended = control_state.end(true);
            if (!control_ended) {
                record(evidence, wrap_lower("runtime.controller.protocol_failed",
                                            "controller.end_control", "unexpected_event",
                                            "completion", std::move(control_ended.error())));
            }
            close_endpoint(channels.controller.control_writer, evidence, "control_close_failed",
                           "completion");
            auto completion_deadline =
                checked_deadline(dependencies.clock, request.lifecycle.completion_grace);
            if (!completion_deadline) {
                record(evidence, std::move(completion_deadline.error()));
            } else {
                auto trailing = read_event(channels.controller.event_reader, dependencies.clock,
                                           *completion_deadline, "completion_timeout");
                if (!trailing) {
                    record(evidence, wrap_lower("runtime.controller.event_read_failed",
                                                "controller.read_completion", "completion_timeout",
                                                "completion", std::move(trailing.error())));
                } else if (std::holds_alternative<ipc::Envelope>(*trailing)) {
                    record(evidence,
                           controller_error("runtime.controller.protocol_failed",
                                            "controller.extra_event", "extra_event", "completion"));
                } else {
                    auto ended = event_state.end();
                    if (!ended) {
                        record(evidence, wrap_lower("runtime.controller.protocol_failed",
                                                    "controller.end_events", "unexpected_eof",
                                                    "completion", std::move(ended.error())));
                    } else {
                        evidence.eof = true;
                    }
                }
                while (!evidence.primary && child.active() &&
                       dependencies.clock.now(dependencies.clock.context) < *completion_deadline) {
                    auto status = child.try_wait();
                    if (status && *status) {
                        evidence.status = std::move(**status);
                        break;
                    }
                    if (!status &&
                        status.error().context.value("classification", "") != "interrupted") {
                        record(evidence,
                               wrap_lower("runtime.controller.wait_failed",
                                          "controller.try_wait_completion", "unreapable_child",
                                          "completion", std::move(status.error())));
                        break;
                    }
                }
                if (!evidence.primary && child.active()) {
                    record(evidence,
                           controller_error("runtime.controller.timeout", "controller.completion",
                                            "completion_timeout", "completion"));
                }
            }
        }

        if (evidence.primary || !evidence.terminal || !evidence.eof || child.active()) {
            if (!evidence.primary) {
                record(evidence, controller_error("runtime.controller.process_failed",
                                                  "controller.incomplete", "runner_exited_early",
                                                  "completion"));
            }
            force_cleanup(child, channels.controller.control_writer,
                          channels.controller.event_reader, request.lifecycle, dependencies.clock,
                          dependencies.fatal, event_state, evidence);
            return failure(std::move(evidence));
        }

        close_endpoint(channels.controller.event_reader, evidence, "event_close_failed", "cleanup");
        if (!evidence.status || !normal_zero(*evidence.status)) {
            record(evidence,
                   controller_error(
                       "runtime.controller.process_failed", "controller.process_consistency",
                       evidence.status && std::holds_alternative<platform_linux::ProcessSignaled>(
                                              *evidence.status)
                           ? "runner_signaled"
                           : "runner_exit_nonzero",
                       "completion"));
        }
        if (!evidence.primary && !evidence.cleanup_errors.empty()) {
            evidence.primary = std::move(evidence.cleanup_errors.front());
        }
        if (evidence.primary)
            return failure(std::move(evidence));
        return ControllerOutcome{
            .request_id = request.request_id,
            .terminal_event = std::move(*evidence.terminal),
            .process_status = std::move(*evidence.status),
            .cancellation_reason = evidence.cancellation,
        };
    } catch (const std::exception&) {
        record(evidence, controller_error("runtime.controller.contained_exception",
                                          "controller.run", "contained_exception"));
    } catch (...) {
        record(evidence, controller_error("runtime.controller.contained_unknown_failure",
                                          "controller.run", "contained_unknown_failure"));
    }
    return failure(std::move(evidence));
}

} // namespace detail

auto run_controller_request(const ControllerRequest& request, CancellationObserver cancellation)
    -> ControllerResult {
    return detail::run_controller_request_with(request, cancellation,
                                               detail::production_controller_dependencies());
}

} // namespace epycbench::runtime
