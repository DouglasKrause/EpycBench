#pragma once

#include "epycbench/scenario/resource_resolution.hpp"

namespace epycbench::scenario::detail {

struct ResolutionPayload {
    std::map<std::string, ResolvedWorkerSet> worker_sets;
    std::vector<ExperimentWorkerReference> experiment_workers;
    std::vector<MatrixWorkerSelectorTemplate> matrix_templates;
    std::vector<ScenarioDiagnostic> diagnostics;
};

struct SingleSelectionPayload {
    std::optional<std::vector<ResolvedProcessingUnit>> processing_units;
    std::vector<ScenarioDiagnostic> diagnostics;
};

struct SelectionOverrides {
    std::optional<std::uint32_t> mandatory_linux_numa_node;
    std::optional<std::string> matrix_worker_node_seed_component;
};

[[nodiscard]] auto verify_resource_topology(const ResourceResolutionContext& context)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto resolve_v1_resources(const DefaultExpandedScenario& scenario,
                                        const ResourceResolutionContext& context)
    -> std::expected<ResolutionPayload, core::Error>;
[[nodiscard]] auto resolve_v1_selection(
    const DefaultExpandedScenario& scenario, const ResourceResolutionContext& context,
    const nlohmann::ordered_json& declaration, std::string pointer, std::string target_name,
    const std::map<std::string, ResolvedWorkerSet>& worker_sets,
    SelectionOverrides overrides = {})
    -> std::expected<SingleSelectionPayload, core::Error>;
[[nodiscard]] auto resources_internal_error(std::string classification,
                                            std::string underlying = {}) -> core::Error;

} // namespace epycbench::scenario::detail
