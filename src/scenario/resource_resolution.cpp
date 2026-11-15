#include "epycbench/scenario/resource_resolution.hpp"

#include "scenario/resource_selector.hpp"

#include <exception>
#include <utility>

namespace epycbench::scenario {

ResourceResolvedScenario::ResourceResolvedScenario(
    DefaultExpandedScenario scenario, topology::TopologyFingerprints fingerprints,
    std::map<std::string, ResolvedWorkerSet> worker_sets,
    std::vector<ExperimentWorkerReference> experiment_workers,
    std::vector<MatrixWorkerSelectorTemplate> matrix_templates)
    : scenario_(std::move(scenario)), fingerprints_(std::move(fingerprints)),
      worker_sets_(std::move(worker_sets)), experiment_workers_(std::move(experiment_workers)),
      matrix_templates_(std::move(matrix_templates)) {}

auto ResourceResolvedScenario::default_expanded() const noexcept -> const DefaultExpandedScenario& {
    return scenario_;
}
auto ResourceResolvedScenario::topology_fingerprints() const noexcept
    -> const topology::TopologyFingerprints& { return fingerprints_; }
auto ResourceResolvedScenario::profile() const noexcept -> std::string_view {
    return resource_resolution_profile;
}
auto ResourceResolvedScenario::worker_sets() const noexcept
    -> const std::map<std::string, ResolvedWorkerSet>& { return worker_sets_; }
auto ResourceResolvedScenario::experiment_workers() const noexcept
    -> const std::vector<ExperimentWorkerReference>& { return experiment_workers_; }
auto ResourceResolvedScenario::matrix_worker_templates() const noexcept
    -> const std::vector<MatrixWorkerSelectorTemplate>& { return matrix_templates_; }

auto resolve_scenario_resources(DefaultExpandedScenario scenario,
                                const ResourceResolutionContext& context)
    -> std::expected<ResourceResolutionResult, core::Error> {
    try {
        auto verified = detail::verify_resource_topology(context);
        if (!verified) return std::unexpected{std::move(verified.error())};
        auto payload = detail::resolve_v1_resources(scenario, context);
        if (!payload) return std::unexpected{std::move(payload.error())};
        if (!payload->diagnostics.empty()) {
            return ResourceResolutionResult{.scenario = std::nullopt,
                                            .diagnostics = std::move(payload->diagnostics)};
        }
        return ResourceResolutionResult{
            .scenario = ResourceResolvedScenario{
                std::move(scenario), context.fingerprints, std::move(payload->worker_sets),
                std::move(payload->experiment_workers), std::move(payload->matrix_templates)},
            .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::resources_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::resources_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
