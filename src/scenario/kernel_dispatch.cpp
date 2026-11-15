#include "epycbench/scenario/kernel_dispatch.hpp"

#include "scenario/scenario_v1_kernels.hpp"

#include <exception>
#include <utility>

namespace epycbench::scenario {

KernelDispatchedScenario::KernelDispatchedScenario(
    MemoryPlacementResolvedScenario scenario,
    std::map<std::string, DispatchedWorkload> workloads,
    std::vector<ExperimentWorkloadBinding> bindings)
    : scenario_{std::move(scenario)}, workloads_{std::move(workloads)},
      bindings_{std::move(bindings)} {}

auto KernelDispatchedScenario::memory_placement_resolved() const noexcept
    -> const MemoryPlacementResolvedScenario& {
    return scenario_;
}

auto KernelDispatchedScenario::workloads() const noexcept
    -> const std::map<std::string, DispatchedWorkload>& {
    return workloads_;
}

auto KernelDispatchedScenario::experiment_workloads() const noexcept
    -> const std::vector<ExperimentWorkloadBinding>& {
    return bindings_;
}

auto KernelDispatchedScenario::dispatch_profile() const noexcept -> std::string_view {
    return kernel_dispatch_profile;
}

auto dispatch_kernel_parameters(MemoryPlacementResolvedScenario scenario)
    -> std::expected<KernelDispatchResult, core::Error> {
    try {
        auto payload = detail::dispatch_v1_kernels(scenario);
        if (!payload) return std::unexpected{std::move(payload.error())};
        if (!payload->diagnostics.empty()) {
            return KernelDispatchResult{.scenario = std::nullopt,
                                        .diagnostics = std::move(payload->diagnostics)};
        }
        return KernelDispatchResult{
            .scenario = KernelDispatchedScenario{
                std::move(scenario), std::move(payload->workloads), std::move(payload->bindings)},
            .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::kernel_dispatch_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{
            detail::kernel_dispatch_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
