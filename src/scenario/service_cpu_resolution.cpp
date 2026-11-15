#include "epycbench/scenario/service_cpu_resolution.hpp"

#include "scenario/resource_selector.hpp"
#include "scenario/scenario_v1_service_cpus.hpp"

#include <exception>
#include <utility>

namespace epycbench::scenario {

ServiceCpuResolvedScenario::ServiceCpuResolvedScenario(
    ResourceResolvedScenario scenario, std::map<std::string, ResolvedServiceCpuPool> pools,
    core::CpuSet reserved_cpus, std::vector<topology::StableCoreId> reserved_cores)
    : scenario_{std::move(scenario)}, pools_{std::move(pools)},
      reserved_cpus_{std::move(reserved_cpus)}, reserved_cores_{std::move(reserved_cores)} {}

auto ServiceCpuResolvedScenario::resource_resolved() const noexcept
    -> const ResourceResolvedScenario& { return scenario_; }
auto ServiceCpuResolvedScenario::service_cpu_pools() const noexcept
    -> const std::map<std::string, ResolvedServiceCpuPool>& { return pools_; }
auto ServiceCpuResolvedScenario::reserved_service_cpus() const noexcept -> const core::CpuSet& {
    return reserved_cpus_;
}
auto ServiceCpuResolvedScenario::reserved_service_cores() const noexcept
    -> const std::vector<topology::StableCoreId>& { return reserved_cores_; }
auto ServiceCpuResolvedScenario::has_dedicated_service_placement() const noexcept -> bool {
    return !pools_.empty();
}
auto ServiceCpuResolvedScenario::resolution_profile() const noexcept -> std::string_view {
    return service_cpu_resolution_profile;
}

auto resolve_service_cpus(ResourceResolvedScenario scenario,
                          const ResourceResolutionContext& context)
    -> std::expected<ServiceCpuResolutionResult, core::Error> {
    try {
        auto verified = detail::verify_resource_topology(context);
        if (!verified) {
            return std::unexpected{detail::service_cpus_internal_error(
                "topology_precondition", verified.error().code.value())};
        }
        if (scenario.topology_fingerprints() != context.fingerprints) {
            return std::unexpected{
                detail::service_cpus_internal_error("retained_fingerprint_mismatch")};
        }
        auto payload = detail::resolve_v1_service_cpus(scenario, context);
        if (!payload) return std::unexpected{std::move(payload.error())};
        if (!payload->diagnostics.empty()) {
            return ServiceCpuResolutionResult{.scenario = std::nullopt,
                                              .diagnostics = std::move(payload->diagnostics)};
        }
        return ServiceCpuResolutionResult{
            .scenario = ServiceCpuResolvedScenario{
                std::move(scenario), std::move(payload->pools), std::move(payload->reserved_cpus),
                std::move(payload->reserved_cores)},
            .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::service_cpus_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::service_cpus_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
