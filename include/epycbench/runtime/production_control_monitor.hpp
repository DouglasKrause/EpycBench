#pragma once

#include "epycbench/platform_linux/thread_identity.hpp"
#include "epycbench/runtime/runner_state_machine.hpp"
#include "epycbench/runtime/service_placement.hpp"

namespace epycbench::runtime {

struct ServiceThreadReadyEvidence {
    ServiceRole role;
    topology::StableProcessingUnitId stable_pu;
    topology::StableCoreId stable_core;
    core::LinuxCpuId assigned_cpu;
    platform_linux::LinuxThreadId tid;
    core::CpuSet observed_affinity;
    core::LinuxCpuId observed_cpu;

    auto operator==(const ServiceThreadReadyEvidence&) const -> bool = default;
};

[[nodiscard]] auto production_runner_control_monitor_factory() noexcept
    -> RunnerControlMonitorFactory;
[[nodiscard]] auto service_thread_readiness(const RunnerControlMonitor& monitor) noexcept
    -> std::expected<std::reference_wrapper<const ServiceThreadReadyEvidence>, core::Error>;

} // namespace epycbench::runtime
