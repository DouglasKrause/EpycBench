#pragma once

#include "epycbench/runtime/pairwise_production_authority.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/runtime/runner_context.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"

namespace epycbench::runtime {

class PairwiseProductionCompositionContext final {
  public:
    PairwiseProductionCompositionContext(const RunnerContext& runner_context,
                                         ProductionRequestIdSource& request_ids) noexcept
        : runner_context_{runner_context}, request_ids_{request_ids} {}

    [[nodiscard]] auto runner_context() const noexcept -> const RunnerContext& {
        return runner_context_;
    }
    [[nodiscard]] auto request_ids() noexcept -> ProductionRequestIdSource& {
        return request_ids_;
    }

  private:
    const RunnerContext& runner_context_;
    ProductionRequestIdSource& request_ids_;
};

[[nodiscard]] auto compose_pairwise_production_inputs(
    const scenario::ScenarioDigestedPlan& plan, const scenario::MatrixCellId& candidate,
    PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<PairwiseProductionAdmission, core::Error>;

[[nodiscard]] auto compose_calibration_only_production_inputs(
    const scenario::ScenarioDigestedPlan& plan,
    const scenario::ComparisonVariantId& candidate,
    PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<CalibrationOnlyProductionAdmission, core::Error>;

[[nodiscard]] auto compose_pairwise_production_inputs(
    const scenario::ScenarioDigestedPlan& plan,
    const scenario::ComparisonVariantId& candidate,
    PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<PairwiseProductionAdmission, core::Error>;

} // namespace epycbench::runtime
