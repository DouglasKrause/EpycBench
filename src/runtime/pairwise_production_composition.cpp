#include "epycbench/runtime/pairwise_production_composition.hpp"

#include "epycbench/runtime/placement_request_projection.hpp"
#include "runtime/pairwise_production_admission_access.hpp"
#include "runtime/calibration_only_production_admission_access.hpp"

#include <exception>
#include <optional>
#include <ranges>
#include <string>

namespace epycbench::runtime {
namespace {

auto composition_error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Pairwise production composition failed",
            .operation = "compose_pairwise_production_inputs",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto comparison_error(std::string classification) -> core::Error {
    return composition_error("runtime.pairwise_production_composition.comparison_failed",
                             std::move(classification));
}

auto calibration_contained_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{
                "runtime.calibration_only_production_admission.contained_failure"},
            .message = "Calibration-only production admission construction failed",
            .operation = "construct_calibration_only_production_admission",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto ratio(const nlohmann::ordered_json& value)
    -> std::expected<scenario::ExactRatio, core::Error> {
    auto result = scenario::ExactRatio::create(value.at("numerator").get<std::uint64_t>(),
                                               value.at("denominator").get<std::uint64_t>());
    if (!result)
        return std::unexpected{comparison_error("comparison_invariant_failed")};
    return *result;
}

// Admission owns metadata only; a rejected local owner retires before return.
// Inspect the actual placement policy's assignments rather than duplicating it.
auto preserve_selected_service_roles(
    std::expected<PairwiseProductionAdmission, core::Error> admission,
    const scenario::ScenarioDigestedPlan& plan)
    -> std::expected<PairwiseProductionAdmission, core::Error> {
    if (!admission)
        return admission;
    const auto& selected = plan.matrix_expanded().kernel_dispatched()
                               .memory_placement_resolved().service_cpu_resolved()
                               .reserved_service_cpus();
    for (const auto& assignment : admission->inputs().service_placement().assignments())
        if (!selected.contains(assignment.linux_cpu))
            return std::unexpected{composition_error(
                "runtime.pairwise_production_composition.service_assignment_failed",
                "service_assignment_outside_selected_pool")};
    return admission;
}

} // namespace

auto compose_pairwise_production_inputs(const scenario::ScenarioDigestedPlan& plan,
                                        const scenario::MatrixCellId& candidate,
                                        PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<PairwiseProductionAdmission, core::Error> {
    try {
        const scenario::ExpandedMatrixExperiment* selected_matrix = nullptr;
        const scenario::ResolvedMatrixCell* baseline_cell = nullptr;
        const scenario::ResolvedMatrixCell* candidate_cell = nullptr;
        std::size_t matches = 0;

        for (const auto& matrix : plan.matrix_expanded().matrix_experiments()) {
            for (const auto& edge : matrix.comparison_edges) {
                if (edge.baseline_cell_index >= matrix.cells.size() ||
                    edge.candidate_cell_index >= matrix.cells.size())
                    return std::unexpected{comparison_error("comparison_invariant_failed")};
                const auto& edge_candidate = matrix.cells[edge.candidate_cell_index];
                if (edge_candidate.id != candidate)
                    continue;
                ++matches;
                selected_matrix = &matrix;
                baseline_cell = &matrix.cells[edge.baseline_cell_index];
                candidate_cell = &edge_candidate;
            }
        }
        if (matches == 0)
            return std::unexpected{comparison_error("comparison_not_found")};
        if (matches != 1 || selected_matrix == nullptr || baseline_cell == nullptr ||
            candidate_cell == nullptr || candidate_cell->id != candidate)
            return std::unexpected{comparison_error("comparison_invariant_failed")};

        auto first = project_placement_request(plan, baseline_cell->id, context.runner_context());
        if (!first)
            return std::unexpected{first.error()};
        auto second = project_placement_request(plan, candidate_cell->id, context.runner_context());
        if (!second)
            return std::unexpected{second.error()};

        auto first_id = context.request_ids().allocate();
        if (!first_id)
            return std::unexpected{first_id.error()};
        auto second_id = context.request_ids().allocate();
        if (!second_id)
            return std::unexpected{second_id.error()};
        if (*first_id == *second_id)
            return std::unexpected{composition_error(
                "runtime.pairwise_production_composition.request_id_invariant_failed",
                "request_id_invariant_failed")};

        const auto& resources = plan.matrix_expanded()
                                    .kernel_dispatched()
                                    .memory_placement_resolved()
                                    .service_cpu_resolved()
                                    .resource_resolved()
                                    .default_expanded()
                                    .value();
        const auto& statistics = resources.at("measurement").at("statistics");
        auto global_effect = ratio(statistics.at("practical_effect"));
        if (!global_effect)
            return std::unexpected{global_effect.error()};
        const auto comparison_effect_path = "/experiments/" +
                                            std::to_string(selected_matrix->experiment_index) +
                                            "/comparison/practical_effect";
        const auto& origins = plan.matrix_expanded()
                                  .kernel_dispatched()
                                  .memory_placement_resolved()
                                  .service_cpu_resolved()
                                  .resource_resolved()
                                  .default_expanded()
                                  .origins();
        const auto effect_origin = origins.find(comparison_effect_path);
        if (effect_origin == origins.end())
            return std::unexpected{comparison_error("comparison_invariant_failed")};
        const auto local_effect =
            effect_origin->second.kind == scenario::ValueOriginKind::matrix_override
                ? std::optional{selected_matrix->comparison.practical_effect}
                : std::nullopt;
        auto statistics_source = StatisticsComparisonPolicySource::create(
            StatisticsExperimentKind::matrix_comparison, selected_matrix->comparison.confidence,
            statistics.at("bootstrap_samples").get<std::uint64_t>(), *global_effect, local_effect);
        if (!statistics_source)
            return std::unexpected{statistics_source.error()};

        const auto seed = selected_matrix->comparison.order_seed.value_or(
            first->execution_inputs().measurement().root_seed());
        return preserve_selected_service_roles(detail::PairwiseProductionAdmissionAccess::create(
            std::move(*first), std::move(*second), *first_id, *second_id, seed,
            std::move(*statistics_source), context.runner_context()), plan);
    } catch (const std::exception&) {
        return std::unexpected{composition_error(
            "runtime.pairwise_production_composition.contained_failure", "contained_exception")};
    } catch (...) {
        return std::unexpected{
            composition_error("runtime.pairwise_production_composition.contained_failure",
                              "contained_unknown_failure")};
    }
}

auto compose_pairwise_production_inputs(const scenario::ScenarioDigestedPlan& plan,
                                        const scenario::ComparisonVariantId& candidate,
                                        PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<PairwiseProductionAdmission, core::Error> {
    try {
        const scenario::ResolvedComparisonExperiment* selected = nullptr;
        std::size_t matches = 0;
        for (const auto& experiment : plan.matrix_expanded().comparison_experiments()) {
            if (experiment.experiment_id != candidate.experiment_id)
                continue;
            if (std::ranges::contains(experiment.candidate_variant_ids, candidate.variant_id)) {
                ++matches;
                selected = &experiment;
            }
        }
        if (matches == 0)
            return std::unexpected{comparison_error("comparison_not_found")};
        if (matches != 1 || selected == nullptr ||
            selected->baseline_variant_id == candidate.variant_id)
            return std::unexpected{comparison_error("comparison_invariant_failed")};

        const scenario::ComparisonVariantId baseline{selected->experiment_id,
                                                     selected->baseline_variant_id};
        auto first = project_placement_request(plan, baseline, context.runner_context());
        if (!first) return std::unexpected{first.error()};
        auto second = project_placement_request(plan, candidate, context.runner_context());
        if (!second) return std::unexpected{second.error()};

        auto first_id = context.request_ids().allocate();
        if (!first_id) return std::unexpected{first_id.error()};
        auto second_id = context.request_ids().allocate();
        if (!second_id) return std::unexpected{second_id.error()};
        if (*first_id == *second_id)
            return std::unexpected{composition_error(
                "runtime.pairwise_production_composition.request_id_invariant_failed",
                "request_id_invariant_failed")};

        const auto& resources = plan.matrix_expanded()
                                    .kernel_dispatched()
                                    .memory_placement_resolved()
                                    .service_cpu_resolved()
                                    .resource_resolved()
                                    .default_expanded()
                                    .value();
        const auto& statistics = resources.at("measurement").at("statistics");
        auto global_effect = ratio(statistics.at("practical_effect"));
        if (!global_effect) return std::unexpected{global_effect.error()};
        const auto local_effect =
            selected->practical_effect_origin.kind == scenario::ValueOriginKind::experiment_override
                ? std::optional{selected->comparison.practical_effect}
                : std::nullopt;
        auto statistics_source = StatisticsComparisonPolicySource::create(
            StatisticsExperimentKind::comparison, selected->comparison.confidence,
            statistics.at("bootstrap_samples").get<std::uint64_t>(), *global_effect, local_effect);
        if (!statistics_source) return std::unexpected{statistics_source.error()};
        const auto seed = selected->comparison.order_seed.value_or(
            first->execution_inputs().measurement().root_seed());
        return preserve_selected_service_roles(detail::PairwiseProductionAdmissionAccess::create(
            std::move(*first), std::move(*second), *first_id, *second_id, seed,
            std::move(*statistics_source), context.runner_context()), plan);
    } catch (const std::exception&) {
        return std::unexpected{composition_error(
            "runtime.pairwise_production_composition.contained_failure", "contained_exception")};
    } catch (...) {
        return std::unexpected{
            composition_error("runtime.pairwise_production_composition.contained_failure",
                              "contained_unknown_failure")};
    }
}

auto compose_calibration_only_production_inputs(
    const scenario::ScenarioDigestedPlan& plan,
    const scenario::ComparisonVariantId& candidate,
    PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<CalibrationOnlyProductionAdmission, core::Error> {
    try {
        const scenario::ResolvedComparisonExperiment* selected = nullptr;
        std::size_t matches = 0;
        for (const auto& experiment : plan.matrix_expanded().comparison_experiments()) {
            if (experiment.experiment_id != candidate.experiment_id)
                continue;
            if (std::ranges::contains(experiment.candidate_variant_ids, candidate.variant_id)) {
                ++matches;
                selected = &experiment;
            }
        }
        if (matches == 0)
            return std::unexpected{comparison_error("comparison_not_found")};
        if (matches != 1 || selected == nullptr ||
            selected->baseline_variant_id == candidate.variant_id)
            return std::unexpected{comparison_error("comparison_invariant_failed")};

        auto first = project_placement_request(
            plan, scenario::ComparisonVariantId{selected->experiment_id,
                                                selected->baseline_variant_id});
        if (!first) return std::unexpected{first.error()};
        auto second = project_placement_request(plan, candidate);
        if (!second) return std::unexpected{second.error()};

        auto first_id = context.request_ids().allocate();
        if (!first_id) return std::unexpected{first_id.error()};
        auto second_id = context.request_ids().allocate();
        if (!second_id) return std::unexpected{second_id.error()};
        if (*first_id == *second_id)
            return std::unexpected{composition_error(
                "runtime.pairwise_production_composition.request_id_invariant_failed",
                "request_id_invariant_failed")};

        const auto seed = selected->comparison.order_seed.value_or(
            first->execution_inputs().measurement().root_seed());
        return detail::CalibrationOnlyProductionAdmissionAccess::create(
            std::move(*first), std::move(*second), *first_id, *second_id, seed,
            context.runner_context());
    } catch (const std::exception&) {
        return std::unexpected{calibration_contained_error("contained_exception")};
    } catch (...) {
        return std::unexpected{calibration_contained_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::runtime
