#pragma once

#include "epycbench/scenario/kernel_dispatch.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::scenario {

namespace detail {
struct ScenarioRetainedPlanTestAccess;
}

inline constexpr std::string_view matrix_expansion_profile = "epycbench.node-pair-matrix.v1";
inline constexpr std::string_view matrix_cell_profile = "epycbench.matrix-cell.v1";

// Optional caller policy. Existing callers retain the original native-domain
// checks; public run supplies finite cumulative limits before output growth.
struct MatrixExpansionLimits {
    std::size_t worker_rows, cells, variants, edges, worker_associations, bytes;
};

struct MatrixCellId {
    std::string experiment_id;
    topology::StableNumaNodeId worker_node;
    topology::StableNumaNodeId memory_node;
    auto operator<=>(const MatrixCellId&) const = default;
};

struct ComparisonVariantId {
    std::string experiment_id;
    std::string variant_id;
    auto operator<=>(const ComparisonVariantId&) const = default;
};

[[nodiscard]] auto matrix_cell_semantic_projection(const MatrixCellId& id)
    -> std::expected<nlohmann::ordered_json, core::Error>;

struct MatrixWorkerRow {
    topology::StableNumaNodeId stable_node_id;
    core::LinuxNumaNodeId linux_node;
    std::vector<ResolvedProcessingUnit> workers;
    core::CpuSet linux_cpus;
    auto operator==(const MatrixWorkerRow&) const -> bool = default;
};

struct DirectedNumaDistance {
    std::uint32_t value{};
    auto operator<=>(const DirectedNumaDistance&) const = default;
};

struct ResolvedMatrixCell {
    MatrixCellId id;
    std::size_t experiment_index{};
    std::size_t worker_row_index{};
    topology::StableNumaNodeId memory_node_stable_id;
    core::LinuxNumaNodeId memory_node;
    ResolvedMemoryPlacement memory_placement;
    DirectedNumaDistance distance;
    auto operator==(const ResolvedMatrixCell&) const -> bool = default;
};

enum class MatrixComparisonOrder {
    randomized_abba,
    randomized_baab,
    randomized_balanced,
};

struct MatrixComparisonConfiguration {
    ExactRatio practical_effect;
    ExactRatio confidence;
    MatrixComparisonOrder order{MatrixComparisonOrder::randomized_abba};
    std::optional<std::uint64_t> order_seed;
    auto operator==(const MatrixComparisonConfiguration&) const -> bool = default;
};

struct MatrixComparisonEdge {
    std::size_t baseline_cell_index{};
    std::size_t candidate_cell_index{};
    auto operator<=>(const MatrixComparisonEdge&) const = default;
};

struct ExpandedMatrixExperiment {
    std::size_t experiment_index{};
    std::string experiment_id;
    std::vector<ResolvedMemoryNode> worker_axis;
    std::vector<ResolvedMemoryNode> memory_axis;
    std::vector<MatrixWorkerRow> worker_rows;
    std::vector<ResolvedMatrixCell> cells;
    MatrixComparisonConfiguration comparison;
    std::vector<MatrixComparisonEdge> comparison_edges;
    auto operator==(const ExpandedMatrixExperiment&) const -> bool = default;
};

struct ResolvedComparisonVariant {
    std::size_t variant_index{};
    std::string variant_id;
    ComparisonMemoryPlacement memory_placement;
    auto operator==(const ResolvedComparisonVariant&) const -> bool = default;
};

struct ResolvedComparisonExperiment {
    std::size_t experiment_index{};
    std::string experiment_id;
    std::string workload_id;
    std::vector<ResolvedProcessingUnit> workers;
    core::CpuSet linux_cpus;
    std::vector<ResolvedComparisonVariant> variants;
    std::string baseline_variant_id;
    std::vector<std::string> candidate_variant_ids;
    MatrixComparisonConfiguration comparison;
    ValueOrigin practical_effect_origin;
    auto operator==(const ResolvedComparisonExperiment&) const -> bool = default;
};

struct MatrixExpansionResult;

class MatrixExpandedScenario {
  public:
    [[nodiscard]] auto kernel_dispatched() const noexcept -> const KernelDispatchedScenario&;
    [[nodiscard]] auto matrix_experiments() const noexcept
        -> const std::vector<ExpandedMatrixExperiment>&;
    [[nodiscard]] auto comparison_experiments() const noexcept
        -> const std::vector<ResolvedComparisonExperiment>&;
    [[nodiscard]] auto expansion_profile() const noexcept -> std::string_view;

  private:
    MatrixExpandedScenario(KernelDispatchedScenario scenario,
                           std::vector<ExpandedMatrixExperiment> matrix_experiments,
                           std::vector<ResolvedComparisonExperiment> comparison_experiments);

    KernelDispatchedScenario scenario_;
    std::vector<ExpandedMatrixExperiment> experiments_;
    std::vector<ResolvedComparisonExperiment> comparison_experiments_;

    friend auto expand_node_pair_matrices(KernelDispatchedScenario,
                                          const ResourceResolutionContext&)
        -> std::expected<MatrixExpansionResult, core::Error>;
    friend auto expand_node_pair_matrices(KernelDispatchedScenario,
                                          const ResourceResolutionContext&,
                                          MatrixExpansionLimits, std::size_t*)
        -> std::expected<MatrixExpansionResult, core::Error>;
    friend struct detail::ScenarioRetainedPlanTestAccess;
};

struct MatrixExpansionResult {
    std::optional<MatrixExpandedScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto expand_node_pair_matrices(KernelDispatchedScenario scenario,
                                             const ResourceResolutionContext& context)
    -> std::expected<MatrixExpansionResult, core::Error>;

[[nodiscard]] auto expand_node_pair_matrices(KernelDispatchedScenario scenario,
                                             const ResourceResolutionContext& context,
                                             MatrixExpansionLimits limits,
                                             std::size_t* charged_bytes = nullptr)
    -> std::expected<MatrixExpansionResult, core::Error>;

} // namespace epycbench::scenario
