#pragma once

#include "epycbench/scenario/scenario_plan_digest.hpp"

#include <cstddef>
#include <expected>
#include <limits>

namespace epycbench::scenario::detail {

// Synchronous borrowed facts, never a successful Scenario stage or a retained owner.
struct RetainedPlanProjectionView {
    const nlohmann::ordered_json& effective;
    const std::map<std::string, ResolvedWorkerSet>& worker_sets;
    const std::vector<ExperimentWorkerReference>& experiment_workers;
    const std::map<std::string, ResolvedServiceCpuPool>& service_cpu_pools;
    const core::CpuSet& reserved_service_cpus;
    const std::vector<topology::StableCoreId>& reserved_service_cores;
    bool dedicated_service_placement;
    const std::vector<ComparisonVariantMemoryPlacement>& comparison_memory;
    const std::map<std::string, DispatchedWorkload>& workloads;
    const std::vector<ExperimentWorkloadBinding>& experiment_workloads;
    const std::vector<ResolvedComparisonExperiment>& comparison_experiments;
    const std::vector<ExpandedMatrixExperiment>& matrix_experiments;
    const topology::TopologyFingerprints& topology_fingerprints;
    std::string_view resource_profile;
    std::string_view service_profile;
    std::string_view memory_profile;
    std::string_view dispatch_profile;
    std::string_view expansion_profile;
};

struct ProjectionInputUsage {
    std::size_t json_nodes{};
    std::size_t typed_entries{};
    std::size_t string_bytes{};
    std::size_t cpu_cardinality{};
    std::size_t depth{};
    std::size_t pairwise_work{};
    auto operator==(const ProjectionInputUsage&) const -> bool = default;
};
using ProjectionInputLimits = ProjectionInputUsage;
inline constexpr auto projection_input_max =
    static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
inline constexpr ProjectionInputLimits projection_input_supported_limits{
    projection_input_max, projection_input_max, projection_input_max,
    projection_input_max, 64U, projection_input_max};
enum class ProjectionInputFailure { invalid_limits, capacity, malformed_input };

[[nodiscard]] auto retained_plan_projection_view(const MatrixExpandedScenario&) noexcept
    -> RetainedPlanProjectionView;
[[nodiscard]] auto inspect_retained_plan_projection(const RetainedPlanProjectionView&,
                                                   ProjectionInputLimits) noexcept
    -> std::expected<ProjectionInputUsage, ProjectionInputFailure>;
[[nodiscard]] auto retained_scenario_semantic_json(const RetainedPlanProjectionView&,
                                                  ProjectionInputLimits)
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto retained_resolved_plan_semantic_json(const RetainedPlanProjectionView&,
                                                       const ScenarioDigest&,
                                                       ProjectionInputLimits)
    -> std::expected<nlohmann::ordered_json, core::Error>;

// Shared implementation: only producer adapters or the checked private entry call these.
namespace projection_implementation {
[[nodiscard]] auto scenario_json(const RetainedPlanProjectionView&)
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto resolved_json(const RetainedPlanProjectionView&, const ScenarioDigest&)
    -> std::expected<nlohmann::ordered_json, core::Error>;
}
} // namespace epycbench::scenario::detail
