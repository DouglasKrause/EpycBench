#pragma once

#include "epycbench/platform_linux/local_ipc.hpp"
#include "epycbench/runtime/runner_internal_mode.hpp"

namespace epycbench::runtime::detail {

struct RunnerEndpointFactory {
    void* context;
    std::expected<platform_linux::LocalIpcEndpoint, core::Error> (*adopt)(void* context,
                                                                         int descriptor);
};

struct RunnerControlCapabilityFactory {
    void* context;
    std::expected<RunnerControlReadCapability, core::Error> (*create)(
        void* context, platform_linux::LocalIpcEndpoint& endpoint);
};

struct InternalErrorRenderer {
    void* context;
    void (*render)(void* context, const core::Error& error) noexcept;
};

[[nodiscard]] auto run_runner_internal_mode_with(
    std::span<const std::string_view> arguments, RunnerSession session,
    RunnerEndpointFactory endpoints, RunnerControlCapabilityFactory capabilities,
    InternalErrorRenderer renderer) -> int;

} // namespace epycbench::runtime::detail
