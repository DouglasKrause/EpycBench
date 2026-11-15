#pragma once

#include "epycbench/runtime/controller_lifecycle.hpp"
#include "runtime/controller_clock.hpp"

namespace epycbench::runtime::detail {

struct ControllerChannelFactory {
    void* context;
    std::expected<platform_linux::LocalIpcChannels, core::Error> (*create)(void* context);
};

struct ControllerProcessSpawner {
    void* context;
    std::expected<platform_linux::ChildProcess, core::Error> (*spawn)(
        void* context, const platform_linux::ChildSpawnRequest& request,
        const platform_linux::RunnerIpcEndpoints& endpoints);
};

struct ControllerFatalInvariant {
    void* context;
    void (*unreapable)(void* context) noexcept;
};

struct ControllerDependencies {
    ControllerChannelFactory channels;
    ControllerProcessSpawner spawner;
    ControllerClock clock;
    ControllerFatalInvariant fatal;
};

[[nodiscard]] auto run_controller_request_with(const ControllerRequest& request,
                                               CancellationObserver cancellation,
                                               ControllerDependencies dependencies)
    -> ControllerResult;

[[nodiscard]] auto production_controller_dependencies() noexcept -> ControllerDependencies;

} // namespace epycbench::runtime::detail
