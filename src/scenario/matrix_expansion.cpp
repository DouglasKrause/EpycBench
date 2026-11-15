#include "epycbench/scenario/matrix_expansion.hpp"
#include "core/json_ownership.hpp"

#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/invariants.hpp"
#include "epycbench/topology/stable_id.hpp"
#include "scenario/resource_selector.hpp"
#include "scenario/scenario_v1_matrix.hpp"

#include <algorithm>
#include <exception>
#include <nlohmann/json.hpp>
#include <ranges>
#include <utility>

namespace epycbench::scenario {
namespace {

template <typename Entry>
auto equal_entries(std::span<const Entry> left, std::span<const Entry> right) -> bool {
    return std::ranges::equal(left, right);
}

auto equal_ids(const topology::StableTopologyIds& left, const topology::StableTopologyIds& right)
    -> bool {
    return left.machine_id() == right.machine_id() &&
           equal_entries(left.packages(), right.packages()) &&
           equal_entries(left.dies(), right.dies()) &&
           equal_entries(left.groups(), right.groups()) &&
           equal_entries(left.cores(), right.cores()) &&
           equal_entries(left.processing_units(), right.processing_units()) &&
           equal_entries(left.caches(), right.caches()) &&
           equal_entries(left.numa_nodes(), right.numa_nodes()) &&
           equal_entries(left.pci_devices(), right.pci_devices());
}

auto verify(const KernelDispatchedScenario& scenario, const ResourceResolutionContext& context)
    -> std::expected<core::ByteCount, core::Error> {
    const auto validation = topology::validate_topology(context.topology);
    if (!validation.valid()) {
        const auto underlying = validation.diagnostics.empty()
                                    ? std::string{}
                                    : validation.diagnostics.front().code.value();
        return std::unexpected{detail::matrix_internal_error("invalid_topology", underlying)};
    }
    auto ids = topology::derive_stable_topology_ids(context.topology);
    if (!ids)
        return std::unexpected{
            detail::matrix_internal_error("stable_id_derivation", ids.error().code.value())};
    if (!equal_ids(*ids, context.stable_ids))
        return std::unexpected{detail::matrix_internal_error("stable_ids_mismatch")};
    auto fingerprints = topology::derive_topology_fingerprints(context.topology, *ids);
    if (!fingerprints)
        return std::unexpected{detail::matrix_internal_error("fingerprint_derivation",
                                                             fingerprints.error().code.value())};
    if (fingerprints->physical != context.fingerprints.physical)
        return std::unexpected{detail::matrix_internal_error("physical_fingerprint_mismatch")};
    if (fingerprints->effective != context.fingerprints.effective)
        return std::unexpected{detail::matrix_internal_error("effective_fingerprint_mismatch")};
    const auto& retained = scenario.memory_placement_resolved()
                               .service_cpu_resolved()
                               .resource_resolved()
                               .topology_fingerprints();
    if (retained.physical != context.fingerprints.physical)
        return std::unexpected{
            detail::matrix_internal_error("retained_physical_fingerprint_mismatch")};
    if (retained.effective != context.fingerprints.effective)
        return std::unexpected{
            detail::matrix_internal_error("retained_effective_fingerprint_mismatch")};
    if (context.topology.machine().page_sizes.empty())
        return std::unexpected{detail::matrix_internal_error("machine_page_sizes_unavailable")};
    return *std::ranges::min_element(context.topology.machine().page_sizes);
}

} // namespace

MatrixExpandedScenario::MatrixExpandedScenario(
    KernelDispatchedScenario scenario, std::vector<ExpandedMatrixExperiment> experiments,
    std::vector<ResolvedComparisonExperiment> comparisons)
    : scenario_{std::move(scenario)}, experiments_{std::move(experiments)},
      comparison_experiments_{std::move(comparisons)} {}

auto MatrixExpandedScenario::kernel_dispatched() const noexcept -> const KernelDispatchedScenario& {
    return scenario_;
}

auto MatrixExpandedScenario::matrix_experiments() const noexcept
    -> const std::vector<ExpandedMatrixExperiment>& {
    return experiments_;
}

auto MatrixExpandedScenario::comparison_experiments() const noexcept
    -> const std::vector<ResolvedComparisonExperiment>& {
    return comparison_experiments_;
}

auto MatrixExpandedScenario::expansion_profile() const noexcept -> std::string_view {
    return matrix_expansion_profile;
}

auto matrix_cell_semantic_projection(const MatrixCellId& id)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    try {
        auto result = nlohmann::ordered_json::object();
        core::detail::JsonCleanup result_cleanup{result};
        result["profile"] = matrix_cell_profile;
        result["experiment"] = id.experiment_id;
        result["worker_node"] = id.worker_node.value();
        result["memory_node"] = id.memory_node.value();
        return nlohmann::ordered_json(std::move(result));
    } catch (const std::exception&) {
        return std::unexpected{detail::matrix_internal_error("cell_projection_failure")};
    }
}

auto expand_node_pair_matrices(KernelDispatchedScenario scenario,
                               const ResourceResolutionContext& context)
    -> std::expected<MatrixExpansionResult, core::Error> {
    try {
        auto base_page = verify(scenario, context);
        if (!base_page)
            return std::unexpected{std::move(base_page.error())};
        auto payload = detail::expand_v1_matrices(scenario, context, *base_page);
        if (!payload)
            return std::unexpected{std::move(payload.error())};
        if (!payload->diagnostics.empty())
            return MatrixExpansionResult{std::nullopt, std::move(payload->diagnostics)};
        return MatrixExpansionResult{MatrixExpandedScenario{std::move(scenario),
                                                            std::move(payload->experiments),
                                                            std::move(payload->comparisons)},
                                     {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::matrix_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::matrix_internal_error("contained_unknown_failure")};
    }
}

auto expand_node_pair_matrices(KernelDispatchedScenario scenario,
                               const ResourceResolutionContext& context, MatrixExpansionLimits limits,
                               std::size_t* charged_bytes)
    -> std::expected<MatrixExpansionResult, core::Error> {
    if (charged_bytes) *charged_bytes = 0;
    try {
        auto base_page = verify(scenario, context);
        if (!base_page)
            return std::unexpected{std::move(base_page.error())};
        std::size_t charge{};
        auto payload = detail::expand_v1_matrices(scenario, context, *base_page, limits, &charge);
        if (!payload)
            return std::unexpected{std::move(payload.error())};
        if (!payload->diagnostics.empty())
            return MatrixExpansionResult{std::nullopt, std::move(payload->diagnostics)};
        auto result = MatrixExpansionResult{MatrixExpandedScenario{std::move(scenario),
                                                            std::move(payload->experiments),
                                                            std::move(payload->comparisons)},
                                     {}};
        if (charged_bytes) *charged_bytes = charge;
        return result;
    } catch (const std::exception&) {
        return std::unexpected{detail::matrix_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::matrix_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
