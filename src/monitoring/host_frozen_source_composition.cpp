#include "monitoring/host_frozen_source_composition.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_environment_access.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
namespace epycbench::monitoring::qualification::detail {
auto HostFrozenSourceComposition::prepare(const RunnerContextAcquisitionSession& session,
                                          runtime::ProductionRequestIdSource& ids,
                                          HostFrozenCompositionStage& stage,
                                          std::optional<core::Error>* raw_failure)
    -> std::expected<HostFrozenSourceComposition, core::Error> {
    stage = HostFrozenCompositionStage::environment;
    auto environment = derive_host_environment_fallible(session, raw_failure);
    core::detail::ExpectedErrorCleanup environment_cleanup{environment};
    if (!environment)
        return std::unexpected{std::move(environment.error())};
    stage = HostFrozenCompositionStage::plan;
    auto plan = detail_HostQualificationPlanAccess::resolve_fallible(session, *environment,
                                                                     raw_failure, &stage);
    core::detail::ExpectedErrorCleanup plan_cleanup{plan};
    if (!plan)
        return std::unexpected{std::move(plan.error())};
    stage = HostFrozenCompositionStage::calibration;
    auto context = detail_RunnerContextAcquisitionAccess::runner_context_fallible(session);
    core::detail::ExpectedErrorCleanup context_cleanup{context};
    if (!context)
        return std::unexpected{std::move(context.error())};
    runtime::PairwiseProductionCompositionContext composition{context->get(), ids};
    auto calibration = compose_host_qualification_calibration_inputs_fallible(
        *plan, session, composition, true, raw_failure);
    core::detail::ExpectedErrorCleanup calibration_cleanup{calibration};
    if (!calibration)
        return std::unexpected{std::move(calibration.error())};
    return HostFrozenSourceComposition{std::move(*environment), std::move(*plan),
                                       std::move(*calibration)};
}
} // namespace epycbench::monitoring::qualification::detail
