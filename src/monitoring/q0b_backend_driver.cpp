#include "epycbench/monitoring/q0b_backend_driver.hpp"

#include "epycbench/monitoring/production_admission_projection.hpp"
#include "epycbench/monitoring/q0b_backend_result.hpp"
#include "epycbench/monitoring/q0b_base_environment_projection.hpp"
#include "epycbench/monitoring/q0b_c0_driver.hpp"
#include "epycbench/monitoring/q0b_production_inputs.hpp"
#include "epycbench/runtime/pairwise_production_composition.hpp"
#include "epycbench/scenario/resolution.hpp"
#include "epycbench/topology/reference_fixture.hpp"
#include "runtime/controller_process_production_authority.hpp"

#include <exception>
#include <utility>

namespace epycbench::monitoring::qualification {
namespace {
auto error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b.backend.driver_failed"},
            .message = "Q0b backend orchestration failed",
            .operation = "monitoring.q0b.backend.run",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto fail(core::Error value) -> Q0bBackendFailure { return {std::move(value), {}}; }
auto fail(Q0bC0DriverFailure value) -> Q0bBackendFailure {
    return {std::move(value.primary), std::move(value.secondary)};
}
auto candidate(const scenario::ScenarioDigestedPlan& plan)
    -> std::expected<scenario::ComparisonVariantId, core::Error> {
    const auto& comparisons = plan.matrix_expanded().comparison_experiments();
    if (comparisons.size() != 1U || comparisons[0].candidate_variant_ids.size() != 1U)
        return std::unexpected{error("comparison_cardinality")};
    return scenario::ComparisonVariantId{comparisons[0].experiment_id,
                                         comparisons[0].candidate_variant_ids[0]};
}
auto resolve(const MonitoringQ0bProductionInputFamilyAuthority& family,
             const runtime::RunnerContext& context)
    -> std::expected<scenario::ScenarioResolutionResult, core::Error> {
    const scenario::ResourceResolutionContext resources{
        context.topology(), context.stable_ids(), context.fingerprints()};
    const auto logical_name = family.scenario_path().generic_string();
    return scenario::resolve_scenario({.logical_name = logical_name,
                                       .contents = family.file().bytes(),
                                       .format = scenario::ScenarioFormat::json}, resources);
}
} // namespace

auto run_q0b_backend_driver(
    const std::filesystem::path& repository_root, std::string_view qualification_id,
    std::string_view collected_at, runtime::ControllerProcessProductionAuthority& authority,
    const Q0bBackendDependencies& dependencies) noexcept
    -> std::expected<Q0bBackendResult, Q0bBackendFailure> {
    try {
        auto loaded = load_q0b_production_inputs(repository_root);
        if (!loaded) return std::unexpected{fail(std::move(loaded.error()))};
        if (!loaded->diagnostics().empty())
            return Q0bBackendResult{Q0bBackendAuthoredDiagnostics{loaded->diagnostics()}};
        if (!loaded->authority()) return std::unexpected{fail(error("missing_input_authority"))};
        const auto& inputs_authority = *loaded->authority();

        if (dependencies.runner_context.acquire == nullptr)
            return std::unexpected{fail(error("missing_runner_context_provider"))};
        auto context = dependencies.runner_context.acquire(dependencies.runner_context.context);
        if (!context) return std::unexpected{fail(std::move(context.error()))};

        auto bandwidth_plan = resolve(inputs_authority.bandwidth(), *context);
        if (!bandwidth_plan) return std::unexpected{fail(std::move(bandwidth_plan.error()))};
        if (!bandwidth_plan->diagnostics.empty())
            return Q0bBackendResult{Q0bBackendAuthoredDiagnostics{
                std::move(bandwidth_plan->diagnostics)}};
        if (!bandwidth_plan->scenario)
            return std::unexpected{fail(error("missing_bandwidth_plan"))};
        auto latency_plan = resolve(inputs_authority.latency(), *context);
        if (!latency_plan) return std::unexpected{fail(std::move(latency_plan.error()))};
        if (!latency_plan->diagnostics.empty())
            return Q0bBackendResult{Q0bBackendAuthoredDiagnostics{
                std::move(latency_plan->diagnostics)}};
        if (!latency_plan->scenario)
            return std::unexpected{fail(error("missing_latency_plan"))};

        auto bandwidth_candidate = candidate(*bandwidth_plan->scenario);
        auto latency_candidate = candidate(*latency_plan->scenario);
        if (!bandwidth_candidate)
            return std::unexpected{fail(std::move(bandwidth_candidate.error()))};
        if (!latency_candidate)
            return std::unexpected{fail(std::move(latency_candidate.error()))};
        runtime::PairwiseProductionCompositionContext composition{*context,
                                                                  authority.request_ids()};
        auto bandwidth_admission = runtime::compose_calibration_only_production_inputs(
            *bandwidth_plan->scenario, *bandwidth_candidate, composition);
        if (!bandwidth_admission)
            return std::unexpected{fail(std::move(bandwidth_admission.error()))};
        auto latency_admission = runtime::compose_calibration_only_production_inputs(
            *latency_plan->scenario, *latency_candidate, composition);
        if (!latency_admission)
            return std::unexpected{fail(std::move(latency_admission.error()))};

        auto fixture = topology::load_reference_fixture_authority(
            {.fixture_root = repository_root / "fixtures/topology/reference-epyc"});
        if (!fixture)
            return std::unexpected{Q0bBackendFailure{
                std::move(fixture.error().primary), std::move(fixture.error().invariant_diagnostics)}};
        auto bandwidth_projection = project_monitoring_production_admission(
            *fixture, *bandwidth_admission, runtime::PairwiseVariant::first);
        auto latency_projection = project_monitoring_production_admission(
            *fixture, *latency_admission, runtime::PairwiseVariant::first);
        if (!bandwidth_projection)
            return std::unexpected{fail(std::move(bandwidth_projection.error()))};
        if (!latency_projection)
            return std::unexpected{fail(std::move(latency_projection.error()))};
        auto bandwidth_base = project_monitoring_q0b_base_environment(*bandwidth_projection);
        auto latency_base = project_monitoring_q0b_base_environment(*latency_projection);
        if (!bandwidth_base) return std::unexpected{fail(std::move(bandwidth_base.error()))};
        if (!latency_base) return std::unexpected{fail(std::move(latency_base.error()))};
        const auto bandwidth_policy = bandwidth_admission->calibration_policy();
        const auto latency_policy = latency_admission->calibration_policy();

        const Q0bC0FamilyInput bandwidth{*bandwidth_admission, *bandwidth_projection,
                                         *bandwidth_base, bandwidth_policy};
        const Q0bC0FamilyInput latency{*latency_admission, *latency_projection,
                                       *latency_base, latency_policy};
        auto calibrations = run_q0b_c0_calibrations(dependencies.operations, bandwidth, latency);
        if (!calibrations) return std::unexpected{fail(std::move(calibrations.error()))};
        auto payload = build_q0b_characterization_payload(
            qualification_id, collected_at, inputs_authority, *bandwidth_base, *calibrations);
        if (!payload) return std::unexpected{fail(std::move(payload.error()))};
        return Q0bBackendResult{std::move(*payload)};
    } catch (const std::exception&) {
        return std::unexpected{fail(error("contained_exception"))};
    } catch (...) {
        return std::unexpected{fail(error("contained_unknown_failure"))};
    }
}

} // namespace epycbench::monitoring::qualification
