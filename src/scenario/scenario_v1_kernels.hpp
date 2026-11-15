#pragma once

#include "epycbench/scenario/kernel_dispatch.hpp"

namespace epycbench::scenario::detail {

struct KernelDispatchPayload {
    std::map<std::string, DispatchedWorkload> workloads;
    std::vector<ExperimentWorkloadBinding> bindings;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto dispatch_v1_kernels(const MemoryPlacementResolvedScenario& scenario)
    -> std::expected<KernelDispatchPayload, core::Error>;
[[nodiscard]] auto kernel_dispatch_internal_error(std::string classification,
                                                  std::string underlying = {}) -> core::Error;

} // namespace epycbench::scenario::detail
