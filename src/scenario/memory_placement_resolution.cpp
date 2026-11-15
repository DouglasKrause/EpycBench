#include "epycbench/scenario/memory_placement_resolution.hpp"

#include "scenario/resource_selector.hpp"
#include "scenario/scenario_v1_memory.hpp"

#include <exception>
#include <utility>

namespace epycbench::scenario {

MemoryPlacementResolvedScenario::MemoryPlacementResolvedScenario(
    ServiceCpuResolvedScenario scenario,
    std::vector<ComparisonVariantMemoryPlacement> comparison,
    std::vector<MatrixMemoryPlacementTemplate> matrix_templates)
    : scenario_{std::move(scenario)}, comparison_{std::move(comparison)},
      matrix_templates_{std::move(matrix_templates)} {}

auto MemoryPlacementResolvedScenario::service_cpu_resolved() const noexcept
    -> const ServiceCpuResolvedScenario& { return scenario_; }
auto MemoryPlacementResolvedScenario::comparison_memory() const noexcept
    -> const std::vector<ComparisonVariantMemoryPlacement>& { return comparison_; }
auto MemoryPlacementResolvedScenario::matrix_memory_templates() const noexcept
    -> const std::vector<MatrixMemoryPlacementTemplate>& { return matrix_templates_; }
auto MemoryPlacementResolvedScenario::resolution_profile() const noexcept -> std::string_view {
    return memory_placement_resolution_profile;
}

auto resolve_memory_placement(ServiceCpuResolvedScenario scenario,
                              const ResourceResolutionContext& context)
    -> std::expected<MemoryPlacementResolutionResult, core::Error> {
    try {
        const auto verified = detail::verify_resource_topology(context);
        if (!verified) {
            auto classification =
                verified.error().context.value("classification", "topology_precondition");
            if (classification == "stable_id_derivation" ||
                classification == "fingerprint_derivation") {
                classification = "invalid_topology";
            }
            return std::unexpected{detail::memory_internal_error(
                std::move(classification),
                verified.error().context.value("underlying_code", verified.error().code.value()))};
        }
        const auto& retained = scenario.resource_resolved().topology_fingerprints();
        if (retained != context.fingerprints)
            return std::unexpected{detail::memory_internal_error("retained_fingerprint_mismatch")};
        if (context.topology.machine().page_sizes.empty())
            return std::unexpected{detail::memory_internal_error(
                "machine_page_sizes_unavailable")};
        const auto base_page_size = context.topology.machine().page_sizes.front();
        if (base_page_size.value() == 0U)
            return std::unexpected{detail::memory_internal_error("invalid_topology")};

        auto payload = detail::resolve_v1_memory_placement(scenario, context, base_page_size);
        if (!payload) return std::unexpected{std::move(payload.error())};
        if (!payload->diagnostics.empty())
            return MemoryPlacementResolutionResult{.scenario = std::nullopt,
                                                   .diagnostics = std::move(payload->diagnostics)};
        return MemoryPlacementResolutionResult{
            .scenario = MemoryPlacementResolvedScenario{
                std::move(scenario), std::move(payload->comparison),
                std::move(payload->matrix_templates)},
            .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::memory_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::memory_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
