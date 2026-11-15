#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/platform_linux/local_ipc.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace epycbench::platform_linux {

namespace detail {
struct ProcessOperations;
struct SpawnOperations;
class ProcessAccess;
} // namespace detail

class LinuxProcessId {
  public:
    [[nodiscard]] static auto create(std::uint64_t value)
        -> std::expected<LinuxProcessId, core::Error>;
    [[nodiscard]] auto value() const noexcept -> std::uint64_t;
    auto operator<=>(const LinuxProcessId&) const noexcept = default;

  private:
    explicit LinuxProcessId(std::uint64_t value) noexcept;
    std::uint64_t value_;
};

struct ProcessExited {
    std::uint8_t code;
    auto operator==(const ProcessExited&) const noexcept -> bool = default;
};

struct ProcessSignaled {
    int signal;
    bool core_dumped;
    auto operator==(const ProcessSignaled&) const noexcept -> bool = default;
};

using ProcessExitStatus = std::variant<ProcessExited, ProcessSignaled>;

enum class ProcessSignal { terminate, kill };

class ChildProcess {
  public:
    ChildProcess(ChildProcess&& other) noexcept;
    auto operator=(ChildProcess&&) -> ChildProcess& = delete;
    ChildProcess(const ChildProcess&) = delete;
    auto operator=(const ChildProcess&) -> ChildProcess& = delete;
    ~ChildProcess() noexcept;

    [[nodiscard]] auto pid() const noexcept -> LinuxProcessId;
    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto wait() -> std::expected<ProcessExitStatus, core::Error>;
    [[nodiscard]] auto try_wait()
        -> std::expected<std::optional<ProcessExitStatus>, core::Error>;
    [[nodiscard]] auto send_signal(ProcessSignal signal)
        -> std::expected<void, core::Error>;

  private:
    ChildProcess(LinuxProcessId pid, const detail::ProcessOperations& operations) noexcept;

    LinuxProcessId pid_;
    bool active_{true};
    void* operation_context_{nullptr};
    detail::RawStatusResult (*wait_pid_)(void*, int, int*, int) noexcept{nullptr};
    detail::RawStatusResult (*send_signal_)(void*, int, int) noexcept{nullptr};

    friend class detail::ProcessAccess;
};

struct ChildSpawnRequest {
    std::string executable;
    std::vector<std::string> arguments;
};

[[nodiscard]] auto spawn_child(const ChildSpawnRequest& request,
                               const RunnerIpcEndpoints& runner_endpoints)
    -> std::expected<ChildProcess, core::Error>;

} // namespace epycbench::platform_linux
