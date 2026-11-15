#pragma once

#include "epycbench/scenario/parser.hpp"
#include "epycbench/scenario/resource_resolution.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"

#include <expected>
#include <optional>
#include <vector>

namespace epycbench::scenario {

struct ScenarioResolutionResult {
    std::optional<ScenarioDigestedPlan> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

struct ScenarioResolutionLimits {
    std::size_t workloads, experiments, selector_sets, variants;
    std::size_t processing_units, numa_nodes, nodes, bytes, canonical_bytes;
    MatrixExpansionLimits expansion;
    std::size_t retained_bytes{256U << 20U};
    std::size_t workers_per_set{1024};
    std::size_t worker_associations{65536};
};

[[nodiscard]] auto resolve_scenario(const ScenarioSource& source,
                                    const ResourceResolutionContext& topology)
    -> std::expected<ScenarioResolutionResult, core::Error>;

[[nodiscard]] auto resolve_scenario(const ScenarioSource& source,
                                    const ResourceResolutionContext& topology,
                                    ScenarioResolutionLimits limits)
    -> std::expected<ScenarioResolutionResult, core::Error>;

} // namespace epycbench::scenario
