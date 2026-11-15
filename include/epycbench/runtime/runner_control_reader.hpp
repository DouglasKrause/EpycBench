#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"

#include <expected>
#include <memory>
#include <variant>

namespace epycbench::platform_linux {
class LocalIpcEndpoint;
}

namespace epycbench::runtime {

namespace detail {
struct RunnerControlReaderAccess;
struct RunnerControlReaderFactory;
} // namespace detail

struct RunnerControlPeerEof final {
    auto operator==(const RunnerControlPeerEof&) const noexcept -> bool = default;
};

struct RunnerControlLocalStop final {
    auto operator==(const RunnerControlLocalStop&) const noexcept -> bool = default;
};

using RunnerControlReadResult =
    std::variant<ipc::Envelope, RunnerControlPeerEof, RunnerControlLocalStop>;

class RunnerControlReadCapability {
  public:
    RunnerControlReadCapability(RunnerControlReadCapability&& other) noexcept;
    RunnerControlReadCapability(const RunnerControlReadCapability&) = delete;
    auto operator=(const RunnerControlReadCapability&) -> RunnerControlReadCapability& = delete;
    auto operator=(RunnerControlReadCapability&&) -> RunnerControlReadCapability& = delete;
    ~RunnerControlReadCapability() noexcept;

    [[nodiscard]] auto read_next() -> std::expected<RunnerControlReadResult, core::Error>;
    [[nodiscard]] auto request_stop() -> std::expected<void, core::Error>;
    [[nodiscard]] auto stop_requested() const noexcept -> bool;
    [[nodiscard]] auto terminal() const noexcept -> bool;

  private:
    struct State;
    explicit RunnerControlReadCapability(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend struct detail::RunnerControlReaderAccess;
    friend struct detail::RunnerControlReaderFactory;
    friend auto make_runner_control_read_capability(platform_linux::LocalIpcEndpoint& endpoint)
        -> std::expected<RunnerControlReadCapability, core::Error>;
};

// Borrows endpoint ownership. The endpoint owner and object storage must outlive every operation
// and any thread performing read_next(). The capability never closes or moves the endpoint.
[[nodiscard]] auto make_runner_control_read_capability(platform_linux::LocalIpcEndpoint& endpoint)
    -> std::expected<RunnerControlReadCapability, core::Error>;

} // namespace epycbench::runtime
