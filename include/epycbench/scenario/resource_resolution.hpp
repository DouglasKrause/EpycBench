#pragma once

#include "epycbench/scenario/default_expansion.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::scenario {

namespace detail {
struct ScenarioRetainedPlanTestAccess;
}

inline constexpr std::string_view resource_resolution_profile =
    "epycbench.resource-resolution.v1";

struct ResourceResolutionContext {
    const topology::TopologySnapshot& topology;
    const topology::StableTopologyIds& stable_ids;
    const topology::TopologyFingerprints& fingerprints;
};

enum class SelectorUnit { processing_unit, core };
enum class SelectorSmtMode { one_per_core, all, explicit_indices };
enum class SelectorOrdering {
    ascending_pu,
    descending_pu,
    compact_cache,
    spread_cache,
    compact_numa,
    spread_numa,
    random,
};

struct SelectorFilters {
    std::vector<std::uint32_t> packages;
    std::vector<std::uint32_t> numa_nodes;
    std::vector<std::uint32_t> dies;
    std::vector<std::uint32_t> cache_domains;
    std::optional<bool> allowed;
    std::optional<bool> online;
    auto operator<=>(const SelectorFilters&) const = default;
};

struct SelectorExclusions {
    std::vector<core::LinuxCpuId> processing_units;
    std::vector<std::uint32_t> cores;
    std::vector<std::string> worker_sets;
    auto operator<=>(const SelectorExclusions&) const = default;
};

struct MatrixWorkerSelectorTemplate {
    std::size_t experiment_index{};
    SelectorUnit unit{SelectorUnit::processing_unit};
    SelectorFilters filters;
    SelectorExclusions exclusions;
    SelectorSmtMode smt{SelectorSmtMode::one_per_core};
    std::vector<std::uint64_t> smt_indices;
    std::optional<std::uint64_t> count;
    SelectorOrdering ordering{SelectorOrdering::ascending_pu};
    std::string source_pointer;
    std::string profile{resource_resolution_profile};
    auto operator<=>(const MatrixWorkerSelectorTemplate&) const = default;
};

struct ResolvedProcessingUnit {
    topology::StableProcessingUnitId stable_id;
    core::LinuxCpuId linux_cpu;
    topology::StableCoreId core_id;
    auto operator<=>(const ResolvedProcessingUnit&) const = default;
};

struct ResolvedWorkerSet {
    std::string name;
    std::vector<ResolvedProcessingUnit> processing_units;
    core::CpuSet linux_cpus;
    std::string source_pointer;
    auto operator==(const ResolvedWorkerSet&) const -> bool = default;
};

struct ExperimentWorkerReference {
    std::size_t experiment_index{};
    std::string worker_set_name;
    std::string source_pointer;
    auto operator<=>(const ExperimentWorkerReference&) const = default;
};

struct ResourceResolutionResult;

class ResourceResolvedScenario {
  public:
    [[nodiscard]] auto default_expanded() const noexcept -> const DefaultExpandedScenario&;
    [[nodiscard]] auto topology_fingerprints() const noexcept
        -> const topology::TopologyFingerprints&;
    [[nodiscard]] auto profile() const noexcept -> std::string_view;
    [[nodiscard]] auto worker_sets() const noexcept -> const std::map<std::string, ResolvedWorkerSet>&;
    [[nodiscard]] auto experiment_workers() const noexcept
        -> const std::vector<ExperimentWorkerReference>&;
    [[nodiscard]] auto matrix_worker_templates() const noexcept
        -> const std::vector<MatrixWorkerSelectorTemplate>&;

  private:
    ResourceResolvedScenario(DefaultExpandedScenario scenario,
                             topology::TopologyFingerprints fingerprints,
                             std::map<std::string, ResolvedWorkerSet> worker_sets,
                             std::vector<ExperimentWorkerReference> experiment_workers,
                             std::vector<MatrixWorkerSelectorTemplate> matrix_templates);

    DefaultExpandedScenario scenario_;
    topology::TopologyFingerprints fingerprints_;
    std::map<std::string, ResolvedWorkerSet> worker_sets_;
    std::vector<ExperimentWorkerReference> experiment_workers_;
    std::vector<MatrixWorkerSelectorTemplate> matrix_templates_;

    friend auto resolve_scenario_resources(DefaultExpandedScenario,
                                           const ResourceResolutionContext&)
        -> std::expected<ResourceResolutionResult, core::Error>;
    friend struct detail::ScenarioRetainedPlanTestAccess;
};

struct ResourceResolutionResult {
    std::optional<ResourceResolvedScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto resolve_scenario_resources(DefaultExpandedScenario scenario,
                                               const ResourceResolutionContext& context)
    -> std::expected<ResourceResolutionResult, core::Error>;

} // namespace epycbench::scenario
