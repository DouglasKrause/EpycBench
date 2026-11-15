#pragma once

#include "epycbench/scenario/service_cpu_resolution.hpp"

namespace epycbench::scenario::detail {

struct ServiceCpuPayload {
    std::map<std::string, ResolvedServiceCpuPool> pools;
    core::CpuSet reserved_cpus;
    std::vector<topology::StableCoreId> reserved_cores;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto resolve_v1_service_cpus(const ResourceResolvedScenario& scenario,
                                           const ResourceResolutionContext& context)
    -> std::expected<ServiceCpuPayload, core::Error>;
[[nodiscard]] auto service_cpus_internal_error(std::string classification,
                                               std::string underlying = {}) -> core::Error;

} // namespace epycbench::scenario::detail
