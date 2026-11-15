#include "epycbench/runtime/runner_state_machine.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::runtime {
namespace {

auto state_error(std::string code, std::string classification, std::string phase,
                 const core::Error* underlying = nullptr) -> core::Error {
    nlohmann::json context{{"classification", std::move(classification)},
                           {"runner_phase", std::move(phase)},
                           {"component", "runner"}};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    std::optional<core::OsError> os_error;
    if (underlying != nullptr && underlying->os_error && underlying->os_error->value > 0)
        os_error =
            core::OsError{.value = underlying->os_error->value, .domain = "errno", .message = ""};
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Runner state machine operation failed",
            .operation = "runner_state_machine",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto session_error(std::string code, std::string classification, std::string phase,
                   const core::Error* underlying = nullptr) -> std::unexpected<core::Error> {
    return std::unexpected{
        state_error(std::move(code), std::move(classification), std::move(phase), underlying)};
}

auto cancel_reason_token(ipc::CancelReason reason) -> std::string_view {
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

auto phase_token(ipc::RunnerPhase phase) -> std::string_view {
    switch (phase) {
    case ipc::RunnerPhase::startup:
        return "startup";
    case ipc::RunnerPhase::placement:
        return "placement";
    case ipc::RunnerPhase::cleanup:
        return "cleanup";
    }
    return "cleanup";
}

auto safe_underlying_code(std::string_view value) -> bool {
    if (value.empty() || value.size() > 128 || value.front() < 'a' || value.front() > 'z')
        return false;
    return std::ranges::all_of(value.substr(1), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '_' || character == '.' || character == '-';
    });
}

auto sanitized_request_error(std::string code, std::string classification,
                             const core::Error* underlying = nullptr) -> core::Error {
    auto result = state_error(std::move(code), std::move(classification), "placement");
    if (underlying != nullptr && safe_underlying_code(underlying->code.value()))
        result.context["underlying_code"] = underlying->code.value();
    if (underlying != nullptr && underlying->os_error && underlying->os_error->value > 0)
        result.os_error =
            core::OsError{.value = underlying->os_error->value, .domain = "errno", .message = ""};
    return result;
}

auto failure_payload(std::string code, std::string classification,
                     const core::Error* underlying = nullptr)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    auto safe = sanitized_request_error(std::move(code), std::move(classification), underlying);
    auto projected = ipc::project_failure(safe);
    if (projected)
        return projected;
    auto fallback = sanitized_request_error("runtime.runner_state.invariant_failed",
                                            "contained_unknown_failure");
    return ipc::project_failure(fallback);
}

struct TerminalDecision {
    ipc::MessageType type;
    nlohmann::ordered_json payload;
};

auto failed_decision(std::string code, std::string classification,
                     const core::Error* underlying = nullptr)
    -> std::expected<TerminalDecision, core::Error> {
    auto payload = failure_payload(std::move(code), std::move(classification), underlying);
    if (!payload)
        return std::unexpected{payload.error()};
    return TerminalDecision{ipc::MessageType::request_failed, std::move(*payload)};
}

auto cancelled_decision(ipc::CancelReason reason, ipc::RunnerPhase phase)
    -> std::expected<TerminalDecision, core::Error> {
    nlohmann::ordered_json payload{{"reason", cancel_reason_token(reason)},
                                   {"phase", phase_token(phase)}};
    auto valid = ipc::validate_payload(ipc::MessageType::request_cancelled, payload);
    if (!valid)
        return std::unexpected{valid.error()};
    return TerminalDecision{ipc::MessageType::request_cancelled, std::move(payload)};
}

class RunnerEventEmitter {
  public:
    RunnerEventEmitter(ipc::RequestId request_id, const ipc::PlacementRequest& request)
        : state_{request_id, request}, request_id_{request_id},
          success_version_{request.transport_version()==ipc::PlacementRequestTransportVersion::v4
             ? ipc::PlacementProofTransportVersion::v2 : ipc::PlacementProofTransportVersion::v1} {}

    auto write_accepted(ipc::ByteWriter& events) -> std::expected<void, core::Error> {
        if (phase_ != Phase::start)
            return session_error("runtime.runner_state.invariant_failed", "invalid_event_state",
                                 "startup");
        auto envelope = ipc::Envelope::create(ipc::MessageType::request_accepted, request_id_,
                                              ipc::SequenceNumber::create(0),
                                              nlohmann::ordered_json{{"accepted", true}});
        if (!envelope)
            return session_error("runtime.runner_state.event_failed",
                                 "request_accepted_construction_failed", "startup",
                                 &envelope.error());
        auto candidate = state_;
        auto validation = candidate.accept(*envelope);
        if (!validation)
            return session_error("runtime.runner_state.event_failed",
                                 "request_accepted_validation_failed", "startup",
                                 &validation.error());
        auto written = ipc::write_frame(events, *envelope);
        if (!written)
            return session_error("runtime.runner_state.event_failed",
                                 "request_accepted_write_failed", "startup", &written.error());
        state_ = std::move(candidate);
        phase_ = Phase::accepted_committed;
        return {};
    }

    auto write_terminal(ipc::ByteWriter& events, TerminalDecision decision)
        -> std::expected<void, core::Error> {
        if (phase_ != Phase::accepted_committed)
            return session_error("runtime.runner_state.invariant_failed", "invalid_event_state",
                                 "cleanup");
        auto envelope = decision.type==ipc::MessageType::placement_succeeded
            ? ipc::Envelope::create(decision.type, request_id_, ipc::SequenceNumber::create(1),
                std::move(decision.payload), success_version_==ipc::PlacementProofTransportVersion::v2
                  ? "epycbench.placement-proof/v2" : "epycbench.placement-proof/v1")
            : ipc::Envelope::create(decision.type, request_id_, ipc::SequenceNumber::create(1),
                                    std::move(decision.payload));
        if (!envelope)
            return session_error("runtime.runner_state.event_failed",
                                 "terminal_construction_failed", "cleanup", &envelope.error());
        auto candidate = state_;
        auto validation = candidate.accept(*envelope);
        if (!validation)
            return session_error("runtime.runner_state.event_failed", "terminal_validation_failed",
                                 "cleanup", &validation.error());
        phase_ = Phase::terminal_attempted;
        auto written = ipc::write_frame(events, *envelope);
        if (!written)
            return session_error("runtime.runner_state.event_failed", "terminal_write_failed",
                                 "cleanup", &written.error());
        state_ = std::move(candidate);
        phase_ = Phase::terminal_committed;
        auto ended = state_.end();
        if (!ended)
            return session_error("runtime.runner_state.invariant_failed", "invalid_event_state",
                                 "cleanup", &ended.error());
        return {};
    }

  private:
    enum class Phase { start, accepted_committed, terminal_attempted, terminal_committed };
    ipc::PlacementEventState state_;
    ipc::RequestId request_id_;
    ipc::PlacementProofTransportVersion success_version_;
    Phase phase_{Phase::start};
};

auto monitor_consistent(const RunnerControlMonitorResult& result,
                        const RunnerCancellation& cancellation) -> bool {
    return result.cancellation_reason() == cancellation.reason();
}

auto run_session(void* context, RunnerControlReadCapability& control, ipc::ByteWriter& events)
    -> std::expected<void, core::Error> {
    if (context == nullptr)
        return session_error("runtime.runner_state.invalid_argument", "missing_state_machine",
                             "startup");
    return static_cast<RunnerStateMachine*>(context)->run(control, events);
}

} // namespace

auto RunnerStateMachine::create(RunnerContextProvider context_provider,
                                RunnerControlMonitorFactory monitor_factory,
                                RunnerExecutionBackend execution_backend) noexcept
    -> std::expected<RunnerStateMachine, core::Error> {
    if (context_provider.acquire == nullptr)
        return session_error("runtime.runner_state.invalid_argument", "missing_context_provider",
                             "startup");
    if (monitor_factory.create == nullptr)
        return session_error("runtime.runner_state.invalid_argument", "missing_monitor_factory",
                             "startup");
    if (execution_backend.execute == nullptr)
        return session_error("runtime.runner_state.invalid_argument", "missing_execution_backend",
                             "startup");
    return RunnerStateMachine{context_provider, monitor_factory, execution_backend};
}

RunnerStateMachine::RunnerStateMachine(RunnerStateMachine&& other) noexcept
    : context_provider_{other.context_provider_}, monitor_factory_{other.monitor_factory_},
      execution_backend_{other.execution_backend_},
      state_{other.state_} {
    if (state_ == State::running)
        std::terminate();
    other.state_ = State::moved_from;
}

auto RunnerStateMachine::session() noexcept -> RunnerSession { return {this, run_session}; }

auto RunnerStateMachine::run(RunnerControlReadCapability& control, ipc::ByteWriter& events)
    -> std::expected<void, core::Error> {
    if (state_ != State::created)
        return session_error("runtime.runner_state.invalid_argument", "already_run", "startup");
    state_ = State::running;
    auto fail = [this](core::Error error) -> std::expected<void, core::Error> {
        state_ = State::failed;
        return std::unexpected{std::move(error)};
    };

    std::optional<RunnerControlMonitor> monitor;
    try {
        auto frame = control.read_next();
        if (!frame)
            return fail(state_error("runtime.runner_state.control_failed", "control_read_failed",
                                    "startup", &frame.error()));
        if (std::holds_alternative<RunnerControlPeerEof>(*frame))
            return fail(state_error("runtime.runner_state.control_failed",
                                    "missing_placement_request", "startup"));
        if (std::holds_alternative<RunnerControlLocalStop>(*frame))
            return fail(state_error("runtime.runner_state.control_failed",
                                    "local_stop_before_placement", "startup"));
        const auto& envelope = std::get<ipc::Envelope>(*frame);
        auto control_state = std::make_unique<ipc::RunnerControlState>();
        auto accepted_control = control_state->accept(envelope);
        if (!accepted_control)
            return fail(state_error("runtime.runner_state.protocol_failed",
                                    "unexpected_control_message", "startup",
                                    &accepted_control.error()));
        auto payload_valid = ipc::validate_payload(envelope.type(), envelope.payload());
        if (!payload_valid)
            return fail(state_error("runtime.runner_state.protocol_failed",
                                    "invalid_placement_request", "startup",
                                    &payload_valid.error()));
        auto request = ipc::parse_placement_request(envelope);
        if (!request)
            return fail(state_error("runtime.runner_state.protocol_failed",
                                    "invalid_placement_request", "startup", &request.error()));

        RunnerEventEmitter emitter{envelope.request_id(), *request};
        auto accepted_event = emitter.write_accepted(events);
        if (!accepted_event)
            return fail(std::move(accepted_event.error()));

        std::optional<RunnerExecutionOutcome> backend_outcome;
        std::optional<core::Error> request_failure;
        bool backend_failure = false;
        bool backend_entered = false;

        auto finish_before_monitor = [&](core::Error error) -> std::expected<void, core::Error> {
            auto decision = failed_decision(
                error.code.value(), error.context.value("classification", "failure"), &error);
            if (!decision)
                return fail(state_error("runtime.runner_state.invariant_failed",
                                        "failure_projection_failed", "cleanup", &decision.error()));
            auto written = emitter.write_terminal(events, std::move(*decision));
            if (!written)
                return fail(std::move(written.error()));
            auto ended = control_state->end(true);
            if (!ended)
                return fail(state_error("runtime.runner_state.invariant_failed",
                                        "control_state_incomplete", "cleanup", &ended.error()));
            state_ = State::completed;
            return {};
        };

        auto context = context_provider_.acquire(context_provider_.context);
        if (!context)
            return finish_before_monitor(
                sanitized_request_error("runtime.runner_state.context_failed",
                                        "context_acquisition_failed", &context.error()));
        auto admitted = admit_placement_request(envelope.request_id(), *request, *context);
        if (!admitted)
            return finish_before_monitor(sanitized_request_error(
                "runtime.runner_state.admission_failed",
                admitted.error().context.value("classification", "admission_failed"),
                &admitted.error()));

        RunnerCancellation cancellation;
        auto created_monitor =
            monitor_factory_.create(monitor_factory_.context, *admitted, std::move(control_state),
                                    control, cancellation);
        if (!created_monitor) {
            auto decision = failed_decision("runtime.runner_state.monitor_failed",
                                            "monitor_factory_failed", &created_monitor.error());
            if (!decision)
                return fail(state_error("runtime.runner_state.invariant_failed",
                                        "failure_projection_failed", "cleanup", &decision.error()));
            auto written = emitter.write_terminal(events, std::move(*decision));
            if (!written)
                return fail(std::move(written.error()));
            state_ = State::completed;
            return {};
        }
        monitor.emplace(std::move(*created_monitor));
        auto monitor_start = monitor->start();
        if (!monitor_start) {
            if (monitor->active())
                return fail(state_error("runtime.runner_state.invariant_failed",
                                        "monitor_start_failed", "startup", &monitor_start.error()));
            auto decision = failed_decision("runtime.runner_state.monitor_failed",
                                            "monitor_start_failed", &monitor_start.error());
            if (!decision)
                return fail(state_error("runtime.runner_state.invariant_failed",
                                        "failure_projection_failed", "cleanup", &decision.error()));
            auto written = emitter.write_terminal(events, std::move(*decision));
            if (!written)
                return fail(std::move(written.error()));
            state_ = State::completed;
            return {};
        }

        if (!cancellation.requested()) {
            backend_entered = true;
            try {
                auto outcome =
                    execution_backend_.execute(execution_backend_.context, *admitted, cancellation);
                if (outcome)
                    backend_outcome.emplace(std::move(*outcome));
                else {
                    backend_failure = true;
                    request_failure =
                        sanitized_request_error("runtime.runner_state.execution_failed",
                                                "backend_execution_failed", &outcome.error());
                }
            } catch (const std::exception&) {
                backend_failure = true;
                request_failure = sanitized_request_error(
                    "runtime.runner_state.contained_exception", "contained_exception");
            } catch (...) {
                backend_failure = true;
                request_failure = sanitized_request_error(
                    "runtime.runner_state.contained_unknown_failure", "contained_unknown_failure");
            }
        }

        auto monitor_result = monitor->stop_and_join();
        if (monitor->active() || !monitor->completed())
            return fail(state_error("runtime.runner_state.invariant_failed",
                                    "monitor_shutdown_failed", "cleanup"));

        std::expected<TerminalDecision, core::Error> decision =
            failed_decision("runtime.runner_state.invariant_failed", "unselected_outcome");
        if (!monitor_result) {
            decision = failed_decision("runtime.runner_state.monitor_failed",
                                       "monitor_protocol_failed", &monitor_result.error());
        } else if (!monitor_consistent(*monitor_result, cancellation)) {
            return fail(state_error("runtime.runner_state.invariant_failed",
                                    "unexpected_monitor_result", "cleanup"));
        } else if (monitor_result->control_eof_observed() && !cancellation.requested()) {
            decision =
                failed_decision("runtime.runner_state.monitor_failed", "unexpected_control_eof");
        } else if (backend_failure && request_failure) {
            decision = failed_decision(
                request_failure->code.value(),
                request_failure->context.value("classification", "execution_failed"),
                &*request_failure);
        } else if (!backend_entered) {
            if (const auto reason = cancellation.reason())
                decision = cancelled_decision(*reason, ipc::RunnerPhase::startup);
            else if (request_failure)
                decision = failed_decision(
                    request_failure->code.value(),
                    request_failure->context.value("classification", "execution_failed"),
                    &*request_failure);
            else
                decision =
                    failed_decision("runtime.runner_state.invariant_failed", "unselected_outcome");
        } else if (!backend_outcome) {
            decision =
                failed_decision("runtime.runner_state.invariant_failed", "unselected_outcome");
        } else if (const auto* failed = std::get_if<RunnerPlacementFailed>(&*backend_outcome)) {
            decision = failed_decision("runtime.runner_state.execution_failed",
                                       "backend_execution_failed", &failed->error);
        } else if (const auto* cancelled =
                       std::get_if<RunnerPlacementCancelled>(&*backend_outcome)) {
            if (!cancellation.requested()) {
                decision = failed_decision("runtime.runner_state.execution_failed",
                                           "backend_spurious_cancellation");
            } else if (cancelled->phase == ipc::RunnerPhase::startup) {
                decision = failed_decision("runtime.runner_state.execution_failed",
                                           "backend_spurious_cancellation");
            } else {
                decision = cancelled_decision(*cancellation.reason(), cancelled->phase);
            }
        } else if (cancellation.requested()) {
            decision = cancelled_decision(*cancellation.reason(), backend_entered
                                                                      ? ipc::RunnerPhase::placement
                                                                      : ipc::RunnerPhase::startup);
        } else {
            const auto& success = std::get<RunnerPlacementSucceeded>(*backend_outcome);
            auto proof_payload =
                ipc::validate_payload(ipc::MessageType::placement_succeeded, success.proof.wire);
            auto parsed_proof = proof_payload ? ipc::parse_placement_proof(success.proof.wire)
                                              : std::expected<ipc::PlacementProof, core::Error>{
                                                    std::unexpected{proof_payload.error()}};
            auto proof_match =
                parsed_proof
                    ? ipc::validate_placement_proof(*request, *parsed_proof)
                    : std::expected<void, core::Error>{std::unexpected{parsed_proof.error()}};
            if (!proof_match)
                decision = failed_decision("runtime.runner_state.execution_failed",
                                           "backend_invalid_proof", &proof_match.error());
            else
                decision =
                    TerminalDecision{ipc::MessageType::placement_succeeded, success.proof.wire};
        }

        if (!decision)
            return fail(state_error("runtime.runner_state.invariant_failed",
                                    "failure_projection_failed", "cleanup", &decision.error()));
        auto terminal = emitter.write_terminal(events, std::move(*decision));
        if (!terminal)
            return fail(std::move(terminal.error()));
        state_ = State::completed;
        return {};
    } catch (const std::exception&) {
        if (monitor && monitor->active())
            (void)monitor->stop_and_join();
        state_ = State::failed;
        return session_error("runtime.runner_state.contained_exception", "contained_exception",
                             "cleanup");
    } catch (...) {
        if (monitor && monitor->active())
            (void)monitor->stop_and_join();
        state_ = State::failed;
        return session_error("runtime.runner_state.contained_unknown_failure",
                             "contained_unknown_failure", "cleanup");
    }
}

} // namespace epycbench::runtime
