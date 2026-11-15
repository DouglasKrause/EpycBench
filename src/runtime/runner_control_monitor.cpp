#include "epycbench/runtime/runner_control_monitor.hpp"

#include <exception>
#include <string>
#include <utility>

namespace epycbench::runtime {
namespace {

auto monitor_error(std::string code, std::string classification, std::string phase,
                   const core::Error* underlying = nullptr) -> core::Error {
    nlohmann::json context{{"classification", std::move(classification)},
                           {"monitor_phase", std::move(phase)},
                           {"component", "runner_control_monitor"}};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    std::optional<core::OsError> os_error;
    if (underlying != nullptr && underlying->os_error)
        os_error =
            core::OsError{.value = underlying->os_error->value, .domain = "errno", .message = ""};
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Runner control monitor operation failed",
            .operation = "runner_control_monitor",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto contained(bool unknown, std::string phase) -> core::Error {
    return monitor_error(unknown ? "runtime.runner_monitor.contained_unknown_failure"
                                 : "runtime.runner_monitor.contained_exception",
                         unknown ? "contained_unknown_failure" : "contained_exception",
                         std::move(phase));
}

auto valid_reason(ipc::CancelReason reason) noexcept -> bool {
    switch (reason) {
    case ipc::CancelReason::user_interrupt:
    case ipc::CancelReason::controller_shutdown:
    case ipc::CancelReason::controller_timeout:
        return true;
    }
    return false;
}

} // namespace

auto RunnerCancellation::encode(ipc::CancelReason reason) noexcept -> std::optional<Code> {
    switch (reason) {
    case ipc::CancelReason::user_interrupt:
        return Code::user_interrupt;
    case ipc::CancelReason::controller_shutdown:
        return Code::controller_shutdown;
    case ipc::CancelReason::controller_timeout:
        return Code::controller_timeout;
    }
    return std::nullopt;
}

auto RunnerCancellation::decode(Code code) noexcept -> std::optional<ipc::CancelReason> {
    switch (code) {
    case Code::none:
        return std::nullopt;
    case Code::user_interrupt:
        return ipc::CancelReason::user_interrupt;
    case Code::controller_shutdown:
        return ipc::CancelReason::controller_shutdown;
    case Code::controller_timeout:
        return ipc::CancelReason::controller_timeout;
    }
    return std::nullopt;
}

auto RunnerCancellation::try_request(ipc::CancelReason reason) noexcept -> bool {
    const auto encoded = encode(reason);
    if (!encoded)
        return false;
    auto expected = Code::none;
    return state_.compare_exchange_strong(expected, *encoded, std::memory_order_release,
                                          std::memory_order_relaxed);
}

auto RunnerCancellation::requested() const noexcept -> bool {
    return state_.load(std::memory_order_acquire) != Code::none;
}

auto RunnerCancellation::reason() const noexcept -> std::optional<ipc::CancelReason> {
    const auto code = state_.load(std::memory_order_acquire);
    const auto result = decode(code);
    if (code != Code::none && !result)
        std::terminate();
    return result;
}

auto RunnerControlMonitorResult::create(std::optional<ipc::CancelReason> cancellation_reason,
                                        bool control_eof_observed) noexcept
    -> std::expected<RunnerControlMonitorResult, core::Error> {
    try {
        if (cancellation_reason && !valid_reason(*cancellation_reason))
            return std::unexpected{monitor_error("runtime.runner_monitor.invalid_argument",
                                                 "invalid_cancellation_state", "result")};
        return RunnerControlMonitorResult{cancellation_reason, control_eof_observed};
    } catch (const std::exception&) {
        return std::unexpected{contained(false, "result")};
    } catch (...) {
        return std::unexpected{contained(true, "result")};
    }
}

auto RunnerControlMonitor::create(void* context, StartFunction start,
                                  StopAndJoinFunction stop_and_join) noexcept
    -> std::expected<RunnerControlMonitor, core::Error> {
    try {
        if (start == nullptr)
            return std::unexpected{monitor_error("runtime.runner_monitor.invalid_argument",
                                                 "missing_start_callback", "construction")};
        if (stop_and_join == nullptr)
            return std::unexpected{monitor_error("runtime.runner_monitor.invalid_argument",
                                                 "missing_stop_callback", "construction")};
        return RunnerControlMonitor{context, start, stop_and_join};
    } catch (const std::exception&) {
        return std::unexpected{contained(false, "construction")};
    } catch (...) {
        return std::unexpected{contained(true, "construction")};
    }
}

auto RunnerControlMonitor::create_owned(std::shared_ptr<void> context, StartFunction start,
                                        StopAndJoinFunction stop_and_join) noexcept
    -> std::expected<RunnerControlMonitor, core::Error> {
    if (!context)
        return std::unexpected{monitor_error("runtime.runner_monitor.invalid_argument",
                                             "missing_context", "construction")};
    auto created = create(context.get(), start, stop_and_join);
    if (!created)
        return created;
    created->owned_context_ = std::move(context);
    return created;
}

RunnerControlMonitor::RunnerControlMonitor(RunnerControlMonitor&& other) noexcept
    : context_{std::exchange(other.context_, nullptr)},
      start_{std::exchange(other.start_, nullptr)},
      stop_and_join_{std::exchange(other.stop_and_join_, nullptr)},
      owned_context_{std::move(other.owned_context_)}, state_{other.state_} {
    other.state_ = State::moved_from;
}

RunnerControlMonitor::~RunnerControlMonitor() noexcept {
    if (destroying_active_is_fatal())
        std::terminate();
}

auto RunnerControlMonitor::start() -> std::expected<void, core::Error> {
    if (state_ != State::created) {
        const auto classification =
            state_ == State::completed ? "already_completed" : "already_started";
        return std::unexpected{
            monitor_error("runtime.runner_monitor.invalid_state", classification, "start")};
    }
    try {
        auto result = start_(context_);
        if (!result) {
            state_ = State::start_failed;
            return std::unexpected{monitor_error("runtime.runner_monitor.start_failed",
                                                 "start_failed", "start", &result.error())};
        }
        state_ = State::active;
        return {};
    } catch (const std::exception&) {
        state_ = State::start_failed;
        return std::unexpected{contained(false, "start")};
    } catch (...) {
        state_ = State::start_failed;
        return std::unexpected{contained(true, "start")};
    }
}

auto RunnerControlMonitor::stop_and_join()
    -> std::expected<RunnerControlMonitorResult, core::Error> {
    if (state_ != State::active) {
        const auto classification =
            state_ == State::completed ? "already_completed" : "not_started";
        return std::unexpected{
            monitor_error("runtime.runner_monitor.invalid_state", classification, "stop_and_join")};
    }
    try {
        auto result = stop_and_join_(context_);
        state_ = State::completed;
        if (!result)
            return std::unexpected{monitor_error("runtime.runner_monitor.stop_failed",
                                                 "stop_failed", "stop_and_join", &result.error())};
        return result;
    } catch (const std::exception&) {
        state_ = State::completed;
        return std::unexpected{contained(false, "stop_and_join")};
    } catch (...) {
        state_ = State::completed;
        return std::unexpected{contained(true, "stop_and_join")};
    }
}

auto RunnerControlMonitor::active() const noexcept -> bool { return state_ == State::active; }
auto RunnerControlMonitor::completed() const noexcept -> bool { return state_ == State::completed; }
auto RunnerControlMonitor::destroying_active_is_fatal() const noexcept -> bool {
    return state_ == State::active;
}

} // namespace epycbench::runtime
