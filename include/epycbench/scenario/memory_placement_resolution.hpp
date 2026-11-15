#pragma once

#include "epycbench/scenario/exact_ratio.hpp"
#include "epycbench/scenario/service_cpu_resolution.hpp"

#include <compare>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

namespace epycbench::scenario {

namespace detail {
struct ScenarioRetainedPlanTestAccess;
}

inline constexpr std::string_view memory_placement_resolution_profile =
    "epycbench.memory-placement-resolution.v1";

enum class MemoryPolicy { bind };
enum class PageMode { base };

struct ResolvedMemoryNode {
    topology::StableNumaNodeId stable_id;
    core::LinuxNumaNodeId linux_node;
    auto operator<=>(const ResolvedMemoryNode&) const = default;
};

struct ResolvedMemoryPlacement {
    MemoryPolicy policy{MemoryPolicy::bind};
    std::vector<ResolvedMemoryNode> nodes;
    std::set<core::LinuxNumaNodeId> linux_nodes;
    PageMode page_mode{PageMode::base};
    core::ByteCount page_size;
    ExactRatio placement_threshold;
    bool prefault{};
    bool verify_before{};
    bool verify_after{};
    std::string source_pointer;
    auto operator==(const ResolvedMemoryPlacement&) const -> bool = default;
};

struct ResolvedPerWorkerMemoryNode {
    ResolvedProcessingUnit worker;
    ResolvedMemoryNode node;
    auto operator==(const ResolvedPerWorkerMemoryNode&) const -> bool = default;
};

struct ResolvedPerWorkerMemoryPlacement {
    MemoryPolicy policy{MemoryPolicy::bind};
    std::vector<ResolvedPerWorkerMemoryNode> worker_nodes;
    PageMode page_mode{PageMode::base};
    core::ByteCount page_size;
    ExactRatio placement_threshold;
    bool prefault{};
    bool verify_before{};
    bool verify_after{};
    std::string source_pointer;
    auto operator==(const ResolvedPerWorkerMemoryPlacement&) const -> bool = default;
};

using ComparisonMemoryPlacement =
    std::variant<ResolvedMemoryPlacement, ResolvedPerWorkerMemoryPlacement>;

struct ComparisonVariantMemoryPlacement {
    std::size_t experiment_index{};
    std::size_t variant_index{};
    ComparisonMemoryPlacement placement;
    auto operator==(const ComparisonVariantMemoryPlacement&) const -> bool = default;
};

struct MatrixMemoryPlacementTemplate {
    std::size_t experiment_index{};
    MemoryPolicy policy{MemoryPolicy::bind};
    PageMode page_mode{PageMode::base};
    core::ByteCount page_size;
    ExactRatio placement_threshold;
    bool prefault{};
    bool verify_before{};
    bool verify_after{};
    std::string source_pointer;
    std::string profile{memory_placement_resolution_profile};
    auto operator==(const MatrixMemoryPlacementTemplate&) const -> bool = default;
};

struct MemoryPlacementResolutionResult;

class MemoryPlacementResolvedScenario {
  public:
    [[nodiscard]] auto service_cpu_resolved() const noexcept
        -> const ServiceCpuResolvedScenario&;
    [[nodiscard]] auto comparison_memory() const noexcept
        -> const std::vector<ComparisonVariantMemoryPlacement>&;
    [[nodiscard]] auto matrix_memory_templates() const noexcept
        -> const std::vector<MatrixMemoryPlacementTemplate>&;
    [[nodiscard]] auto resolution_profile() const noexcept -> std::string_view;

  private:
    MemoryPlacementResolvedScenario(
        ServiceCpuResolvedScenario scenario,
        std::vector<ComparisonVariantMemoryPlacement> comparison,
        std::vector<MatrixMemoryPlacementTemplate> matrix_templates);

    ServiceCpuResolvedScenario scenario_;
    std::vector<ComparisonVariantMemoryPlacement> comparison_;
    std::vector<MatrixMemoryPlacementTemplate> matrix_templates_;

    friend auto resolve_memory_placement(ServiceCpuResolvedScenario,
                                         const ResourceResolutionContext&)
        -> std::expected<MemoryPlacementResolutionResult, core::Error>;
    friend struct detail::ScenarioRetainedPlanTestAccess;
};

struct MemoryPlacementResolutionResult {
    std::optional<MemoryPlacementResolvedScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto resolve_memory_placement(ServiceCpuResolvedScenario scenario,
                                            const ResourceResolutionContext& context)
    -> std::expected<MemoryPlacementResolutionResult, core::Error>;

} // namespace epycbench::scenario
