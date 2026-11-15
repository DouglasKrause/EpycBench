#pragma once

#include "monitoring/host_frozen_source_launch.hpp"

namespace epycbench::monitoring::qualification::detail {

// Direct governed baseline parent; this entry conveys no frozen Q5/Q6/Q7 grant.
[[nodiscard]] auto is_host_q0b_governed_parent(int argc, char* argv[]) noexcept -> bool;
[[nodiscard]] auto run_host_q0b_governed_parent(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority&, runtime::RunnerCancellation&,
    HostFrozenSourceLaunchClock&, std::uint64_t original_main_entry_ns) noexcept -> int;

// Executable-local synthetic containment seam. The production entry always
// supplies no observer and cannot select one through arguments or environment.
struct HostQ0bGovernedParentObserver final {
    void* context{};
    platform_linux::detail::RawStatusResult (*before_gate)(void*, int actual_collector_pid,
        std::uint64_t original_launch_origin, std::uint64_t original_launch_end) noexcept{};
};
[[nodiscard]] auto run_host_q0b_governed_parent_observed(int argc, char* argv[],
    runtime::ControllerProcessProductionAuthority&, runtime::RunnerCancellation&,
    HostFrozenSourceLaunchClock&, std::uint64_t original_main_entry_ns,
    const HostQ0bGovernedParentObserver&) noexcept -> int;

} // namespace epycbench::monitoring::qualification::detail
