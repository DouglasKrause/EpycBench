#include "epycbench/platform_linux/child_process.hpp"

#include "platform_linux/process_ipc_operations.hpp"

#include <cerrno>
#include <csignal>
#include <exception>
#include <limits>
#include <optional>
#include <spawn.h>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <system_error>
#include <utility>

extern char** environ;

namespace epycbench::platform_linux {
namespace detail {
namespace {

[[nodiscard]] auto process_error(std::string_view code, std::string_view message,
                                 std::string_view operation,
                                 std::string_view classification,
                                 std::optional<int> error_number = std::nullopt,
                                 std::string_view process_state = {}) -> core::Error {
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(
            std::error_code{*error_number, std::generic_category()});
    }
    auto context = nlohmann::json{{"classification", classification}};
    if (!process_state.empty()) context["process_state"] = process_state;
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto spawn_classification(int error_number) -> std::string_view {
    switch (error_number) {
    case ENOENT: return "executable_not_found";
    case EACCES: return "permission_denied";
    case ENOEXEC: return "invalid_executable";
    case E2BIG: return "argument_too_large";
    case EAGAIN:
    case ENOMEM: return "resource_limit";
    default: return "native_failure";
    }
}

[[nodiscard]] auto parse_status(int status) -> std::expected<ProcessExitStatus, core::Error> {
    constexpr std::string_view operation = "ChildProcess.wait_status";
    if (WIFEXITED(status)) {
        const auto code = WEXITSTATUS(status);
        if (code < 0 || code > 255) {
            return std::unexpected{process_error(
                "linux.process.invalid_status", "Child exit status is invalid", operation,
                "invalid_exit_status")};
        }
        return ProcessExitStatus{ProcessExited{static_cast<std::uint8_t>(code)}};
    }
    if (WIFSIGNALED(status)) {
        const auto signal = WTERMSIG(status);
        if (signal <= 0) {
            return std::unexpected{process_error(
                "linux.process.invalid_status", "Child signal status is invalid", operation,
                "invalid_exit_status")};
        }
        return ProcessExitStatus{ProcessSignaled{
            .signal = signal,
            .core_dumped = WCOREDUMP(status) != 0,
        }};
    }
    return std::unexpected{process_error(
        "linux.process.invalid_status", "Child did not return a terminal status", operation,
        "invalid_exit_status")};
}

[[nodiscard]] auto validate_request(const ChildSpawnRequest& request)
    -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "spawn_child";
    if (request.executable.empty() ||
        request.executable.find('\0') != std::string::npos) {
        return std::unexpected{process_error(
            "linux.process.invalid_argument", "Child executable is invalid", operation,
            "invalid_executable")};
    }
    if (request.arguments.empty() || request.arguments.front().empty()) {
        return std::unexpected{process_error(
            "linux.process.invalid_argument", "Child argument vector is invalid", operation,
            "invalid_arguments")};
    }
    for (const auto& argument : request.arguments) {
        if (argument.find('\0') != std::string::npos) {
            return std::unexpected{process_error(
                "linux.process.invalid_argument", "Child argument is invalid", operation,
                "invalid_arguments")};
        }
    }
    return {};
}

class SpawnActionsGuard {
  public:
    SpawnActionsGuard(const SpawnOperations& operations, void* actions) noexcept
        : operations_{&operations}, actions_{actions} {}
    SpawnActionsGuard(const SpawnActionsGuard&) = delete;
    auto operator=(const SpawnActionsGuard&) -> SpawnActionsGuard& = delete;
    ~SpawnActionsGuard() noexcept {
        static_cast<void>(operations_->destroy(operations_->context, actions_));
    }

  private:
    const SpawnOperations* operations_;
    void* actions_;
};

} // namespace

auto ProcessAccess::spawn(const ChildSpawnRequest& request,
                          const RunnerIpcEndpoints& endpoints,
                          const SpawnOperations& spawn,
                          const ProcessOperations& process)
    -> std::expected<ChildProcess, core::Error> {
    constexpr std::string_view operation = "spawn_child";
    try {
        auto valid = validate_request(request);
        if (!valid) return std::unexpected{std::move(valid.error())};
        if (!endpoints.control_reader.active() || !endpoints.event_writer.active()) {
            return std::unexpected{process_error(
                "linux.process.invalid_argument", "Runner IPC endpoints are inactive", operation,
                "endpoint_closed")};
        }
        const auto control = endpoints.control_reader.descriptor_.native_handle();
        const auto event = endpoints.event_writer.descriptor_.native_handle();
        if (control < 5 || event < 5) {
            return std::unexpected{process_error(
                "linux.process.invalid_argument", "Runner IPC endpoints are not normalized",
                operation, "invalid_descriptor")};
        }

        std::vector<char*> arguments;
        arguments.reserve(request.arguments.size() + 1U);
        for (const auto& argument : request.arguments) {
            arguments.push_back(const_cast<char*>(argument.c_str()));
        }
        arguments.push_back(nullptr);

        void* actions{};
        const auto initialized = spawn.initialize(spawn.context, &actions);
        if (initialized.value != 0 || initialized.error_number != 0 || actions == nullptr) {
            const auto number = initialized.error_number != 0 ? initialized.error_number
                                                               : initialized.value;
            return std::unexpected{process_error(
                "linux.process.spawn_setup_failed", "Child spawn setup failed", operation,
                "spawn_setup_failed", number == 0 ? std::optional<int>{}
                                                   : std::optional<int>{number})};
        }
        const SpawnActionsGuard actions_guard{spawn, actions};

        std::optional<core::Error> primary;
        const auto first = spawn.add_dup2(spawn.context, actions, control, 3);
        if (first.value != 0 || first.error_number != 0) {
            const auto number = first.error_number != 0 ? first.error_number : first.value;
            primary = process_error("linux.process.spawn_setup_failed",
                                    "Child spawn descriptor setup failed", operation,
                                    "spawn_setup_failed", number);
        }
        if (!primary) {
            const auto second = spawn.add_dup2(spawn.context, actions, event, 4);
            if (second.value != 0 || second.error_number != 0) {
                const auto number = second.error_number != 0 ? second.error_number : second.value;
                primary = process_error("linux.process.spawn_setup_failed",
                                        "Child spawn descriptor setup failed", operation,
                                        "spawn_setup_failed", number);
            }
        }
        if (!primary) {
            const auto close_from = spawn.add_close_from(spawn.context, actions, 5);
            if (close_from.value != 0 || close_from.error_number != 0) {
                const auto number = close_from.error_number != 0
                                        ? close_from.error_number : close_from.value;
                primary = process_error("linux.process.spawn_setup_failed",
                                        "Child spawn close-from setup failed", operation,
                                        "spawn_setup_failed", number);
            }
        }

        int native_pid{};
        if (!primary) {
            const auto spawned = spawn.spawn(spawn.context, &native_pid,
                                             request.executable.c_str(), actions,
                                             arguments.data(), environ);
            if (spawned.value != 0 || spawned.error_number != 0) {
                const auto number = spawned.error_number != 0
                                        ? spawned.error_number : spawned.value;
                primary = process_error("linux.process.spawn_failed", "Child spawn failed",
                                        operation, spawn_classification(number), number);
            }
        }

        if (primary) return std::unexpected{std::move(*primary)};

        if (native_pid <= 0) {
            return std::unexpected{process_error(
                "linux.process.spawn_failed", "Child spawn returned an invalid process ID",
                operation, "unexpected_native_result")};
        }
        auto pid = LinuxProcessId::create(static_cast<std::uint64_t>(native_pid));
        if (!pid) return std::unexpected{std::move(pid.error())};
        return ChildProcess{*pid, process};
    } catch (const std::exception&) {
        return std::unexpected{process_error(
            "linux.process.contained_exception", "Child spawn contained an exception", operation,
            "contained_exception")};
    } catch (...) {
        return std::unexpected{process_error(
            "linux.process.contained_unknown_failure",
            "Child spawn contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto ProcessAccess::make_child(LinuxProcessId pid,
                               const ProcessOperations& operations) noexcept -> ChildProcess {
    return ChildProcess{pid, operations};
}

auto parse_fixed_process_status(int status) noexcept
    -> std::expected<ProcessExitStatus, FixedProcessFailure> {
    if (WIFEXITED(status)) {
        const auto code = WEXITSTATUS(status);
        if (code >= 0 && code <= 255)
            return ProcessExited{static_cast<std::uint8_t>(code)};
    } else if (WIFSIGNALED(status) && WTERMSIG(status) > 0) {
        return ProcessSignaled{WTERMSIG(status), WCOREDUMP(status) != 0};
    }
    return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_status, 0}};
}

auto ProcessAccess::wait_fixed(ChildProcess& owner, bool blocking) noexcept
    -> std::expected<std::optional<ProcessExitStatus>, FixedProcessFailure> {
    using Kind = FixedProcessFailureKind;
    if (!owner.active_ || !owner.wait_pid_)
        return std::unexpected{FixedProcessFailure{Kind::invalid_state, 0}};
    int status{};
    const auto result = owner.wait_pid_(owner.operation_context_, static_cast<int>(owner.pid_.value()),
                                        &status, blocking ? 0 : WNOHANG);
    if (!blocking && result.value == 0 && result.error_number == 0)
        return std::optional<ProcessExitStatus>{};
    if (result.value == static_cast<long>(owner.pid_.value()) && result.error_number == 0) {
        auto parsed = parse_fixed_process_status(status);
        if (!parsed) return std::unexpected{parsed.error()};
        owner.active_ = false;
        return std::optional<ProcessExitStatus>{*parsed};
    }
    if (result.value > 0 && result.error_number == 0)
        return std::unexpected{FixedProcessFailure{Kind::wrong_child, 0}};
    if (result.value == -1 && result.error_number > 0)
        return std::unexpected{FixedProcessFailure{
            result.error_number == EINTR ? Kind::interrupted : Kind::native_failure, result.error_number}};
    return std::unexpected{FixedProcessFailure{Kind::malformed_result, 0}};
}
auto ProcessAccess::signal_fixed(ChildProcess& owner, ProcessSignal signal) noexcept
    -> std::expected<void, FixedProcessFailure> {
    using Kind = FixedProcessFailureKind;
    if (!owner.active_ || !owner.send_signal_)
        return std::unexpected{FixedProcessFailure{Kind::invalid_state, 0}};
    if (signal != ProcessSignal::terminate && signal != ProcessSignal::kill)
        return std::unexpected{FixedProcessFailure{Kind::invalid_argument, 0}};
    const auto result = owner.send_signal_(owner.operation_context_, static_cast<int>(owner.pid_.value()),
                                           signal == ProcessSignal::terminate ? SIGTERM : SIGKILL);
    if (result.value == 0 && result.error_number == 0) return {};
    if (result.value == -1 && result.error_number > 0)
        return std::unexpected{FixedProcessFailure{
            result.error_number == EINTR ? Kind::interrupted : Kind::native_failure, result.error_number}};
    return std::unexpected{FixedProcessFailure{Kind::malformed_result, 0}};
}

} // namespace detail

LinuxProcessId::LinuxProcessId(std::uint64_t value) noexcept : value_{value} {}

auto LinuxProcessId::create(std::uint64_t value)
    -> std::expected<LinuxProcessId, core::Error> {
    constexpr std::string_view operation = "LinuxProcessId.create";
    try {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max());
        if (value == 0 || value > maximum) {
            return std::unexpected{detail::process_error(
                "linux.process.invalid_argument", "Linux process ID is invalid", operation,
                "invalid_process_id")};
        }
        return LinuxProcessId{value};
    } catch (const std::exception&) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_exception", "Process ID creation contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_unknown_failure",
            "Process ID creation contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto LinuxProcessId::value() const noexcept -> std::uint64_t { return value_; }

ChildProcess::ChildProcess(LinuxProcessId pid,
                           const detail::ProcessOperations& operations) noexcept
    : pid_{pid}, operation_context_{operations.context}, wait_pid_{operations.wait_pid},
      send_signal_{operations.send_signal} {}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : pid_{other.pid_}, active_{std::exchange(other.active_, false)},
      operation_context_{std::exchange(other.operation_context_, nullptr)},
      wait_pid_{std::exchange(other.wait_pid_, nullptr)},
      send_signal_{std::exchange(other.send_signal_, nullptr)} {}

ChildProcess::~ChildProcess() noexcept {
    if (active_) std::terminate();
}

auto ChildProcess::pid() const noexcept -> LinuxProcessId { return pid_; }
auto ChildProcess::active() const noexcept -> bool { return active_; }

auto ChildProcess::wait() -> std::expected<ProcessExitStatus, core::Error> {
    constexpr std::string_view operation = "ChildProcess.wait";
    if (!active_) {
        return std::unexpected{detail::process_error(
            "linux.process.invalid_state", "Child process is already reaped", operation,
            "already_reaped", std::nullopt, "reaped")};
    }
    try {
        while (true) {
            int status{};
            const auto result = wait_pid_(operation_context_, static_cast<int>(pid_.value()),
                                          &status, 0);
            if (result.value == -1 && result.error_number == EINTR) continue;
            if (result.value == static_cast<long>(pid_.value()) && result.error_number == 0) {
                auto parsed = detail::parse_status(status);
                if (!parsed) return std::unexpected{std::move(parsed.error())};
                active_ = false;
                return parsed;
            }
            if (result.value > 0) {
                return std::unexpected{detail::process_error(
                    "linux.process.wait_failed", "Unexpected child was observed", operation,
                    "unexpected_child", std::nullopt, "active")};
            }
            if (result.value == -1 && result.error_number != 0) {
                return std::unexpected{detail::process_error(
                    "linux.process.wait_failed", "Child wait failed", operation,
                    result.error_number == ECHILD ? "child_not_found" : "native_failure",
                    result.error_number, "active")};
            }
            return std::unexpected{detail::process_error(
                "linux.process.wait_failed", "Child wait returned invalid status", operation,
                "unexpected_native_result", std::nullopt, "active")};
        }
    } catch (const std::exception&) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_exception", "Child wait contained an exception", operation,
            "contained_exception", std::nullopt, "active")};
    } catch (...) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_unknown_failure",
            "Child wait contained an unknown failure", operation,
            "contained_unknown_failure", std::nullopt, "active")};
    }
}

auto ChildProcess::try_wait()
    -> std::expected<std::optional<ProcessExitStatus>, core::Error> {
    constexpr std::string_view operation = "ChildProcess.try_wait";
    if (!active_) {
        return std::unexpected{detail::process_error(
            "linux.process.invalid_state", "Child process is already reaped", operation,
            "already_reaped", std::nullopt, "reaped")};
    }
    try {
        int status{};
        const auto result = wait_pid_(operation_context_, static_cast<int>(pid_.value()),
                                      &status, WNOHANG);
        if (result.value == 0 && result.error_number == 0) {
            return std::optional<ProcessExitStatus>{};
        }
        if (result.value == static_cast<long>(pid_.value()) && result.error_number == 0) {
            auto parsed = detail::parse_status(status);
            if (!parsed) return std::unexpected{std::move(parsed.error())};
            active_ = false;
            return std::optional<ProcessExitStatus>{std::move(*parsed)};
        }
        if (result.value > 0) {
            return std::unexpected{detail::process_error(
                "linux.process.wait_failed", "Unexpected child was observed", operation,
                "unexpected_child", std::nullopt, "active")};
        }
        if (result.value == -1 && result.error_number != 0) {
            const auto classification = result.error_number == EINTR
                                            ? "interrupted"
                                            : result.error_number == ECHILD
                                                  ? "child_not_found" : "native_failure";
            return std::unexpected{detail::process_error(
                "linux.process.wait_failed", "Child nonblocking wait failed", operation,
                classification, result.error_number, "active")};
        }
        return std::unexpected{detail::process_error(
            "linux.process.wait_failed", "Child wait returned invalid status", operation,
            "unexpected_native_result", std::nullopt, "active")};
    } catch (const std::exception&) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_exception", "Child wait contained an exception", operation,
            "contained_exception", std::nullopt, "active")};
    } catch (...) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_unknown_failure",
            "Child wait contained an unknown failure", operation,
            "contained_unknown_failure", std::nullopt, "active")};
    }
}

auto ChildProcess::send_signal(ProcessSignal signal)
    -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "ChildProcess.send_signal";
    if (!active_) {
        return std::unexpected{detail::process_error(
            "linux.process.invalid_state", "Child process is already reaped", operation,
            "already_reaped", std::nullopt, "reaped")};
    }
    try {
        const auto native_signal = signal == ProcessSignal::terminate ? SIGTERM : SIGKILL;
        const auto result = send_signal_(operation_context_, static_cast<int>(pid_.value()),
                                         native_signal);
        if (result.value == 0 && result.error_number == 0) return {};
        if (result.value == -1 && result.error_number != 0) {
            std::string_view classification = "native_failure";
            if (result.error_number == ESRCH) classification = "process_not_found";
            else if (result.error_number == EPERM) classification = "permission_denied";
            else if (result.error_number == EINVAL) classification = "invalid_signal";
            return std::unexpected{detail::process_error(
                "linux.process.signal_failed", "Child signal delivery failed", operation,
                classification, result.error_number, "active")};
        }
        return std::unexpected{detail::process_error(
            "linux.process.signal_failed", "Child signal returned invalid status", operation,
            "unexpected_native_result", std::nullopt, "active")};
    } catch (const std::exception&) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_exception", "Child signal contained an exception", operation,
            "contained_exception", std::nullopt, "active")};
    } catch (...) {
        return std::unexpected{detail::process_error(
            "linux.process.contained_unknown_failure",
            "Child signal contained an unknown failure", operation,
            "contained_unknown_failure", std::nullopt, "active")};
    }
}

auto spawn_child(const ChildSpawnRequest& request,
                 const RunnerIpcEndpoints& runner_endpoints)
    -> std::expected<ChildProcess, core::Error> {
    return detail::ProcessAccess::spawn(request, runner_endpoints,
                                        detail::production_spawn_operations(),
                                        detail::production_process_operations());
}

} // namespace epycbench::platform_linux
