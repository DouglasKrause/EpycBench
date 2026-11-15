#include "monitoring/host_frozen_observation_admission.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_environment_access.hpp"
#include "monitoring/host_q0b_v2_projection_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "runtime/ipc/placement_request_construction_access.hpp"
#include "runtime/pairwise_v2_contextual_admission_classification.hpp"
#include "runtime/production_admission_policy.hpp"
#include "runtime/production_request_id_access.hpp"

#include <limits>
#include <new>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = HostFrozenAdmissionReason;
auto failure(Reason reason) noexcept -> HostFrozenAdmissionFailure { return {reason, {}}; }
auto failure(Reason reason, core::Error error) noexcept -> HostFrozenAdmissionFailure {
    return {reason, std::move(error)};
}
auto binding_failure(HostFrozenBindingCheckpoint checkpoint) noexcept -> HostFrozenAdmissionFailure {
    return {Reason::profile_binding, {}, checkpoint};
}
auto valid(HostFrozenConfiguration configuration) noexcept -> bool {
    switch (configuration) {
    case HostFrozenConfiguration::c0: case HostFrozenConfiguration::c1:
    case HostFrozenConfiguration::c2: case HostFrozenConfiguration::c3:
    case HostFrozenConfiguration::c4: return true;
    }
    return false;
}
auto family_binding(const MonitoringQ1Q0FamilyAdmission& expected,
                    const MonitoringHostAdmittedProductionFacts& actual,
                    std::string_view name, const core::Sha256Digest& base) noexcept -> bool {
    return expected.family == name &&
           expected.schema == "epycbench.monitoring-admitted-production-facts/v2" &&
           expected.variant_role == "first" && expected.sha256 == actual.digest() &&
           expected.base_environment_sha256 == base;
}
auto exact_service(const runtime::ServicePlacementPlan& service,
                   const MonitoringHostQualificationPlan& plan, bool measurement) noexcept -> bool {
    const auto roles = service.assignments();
    const auto expected = plan.service_cores();
    if (expected.size() != 2 || roles.size() != (measurement ? 2U : 1U)) return false;
    for (const auto& role : roles) {
        const bool control = role.role == runtime::ServiceRole::control_monitor;
        if (!control && role.role != runtime::ServiceRole::measurement_monitor) return false;
        const auto& selected = expected[control ? 0U : 1U];
        if (selected.role != (control ? "control" : "measurement") ||
            role.stable_core != selected.stable_core_id ||
            role.stable_pu != selected.execution_stable_pu_id ||
            role.linux_cpu != selected.execution_linux_cpu) return false;
    }
    return true;
}
} // namespace

auto HostFrozenObservationAdmission::monitoring_wire(HostFrozenConfiguration configuration) -> nlohmann::json {
    using C = HostFrozenConfiguration;
    using Json = nlohmann::json;
    auto required = Json::array();
    core::detail::JsonCleanup required_cleanup{required};
    const auto append = [&](std::string_view name) { core::detail::append_json(required, Json(name)); };
    if (configuration == C::c1 || configuration == C::c4) append("page_faults");
    if (configuration == C::c2 || configuration == C::c4) append("context_switches");
    if (configuration == C::c3 || configuration == C::c4) { append("temperature"); append("frequency"); }
    const bool scheduled = configuration == C::c2 || configuration == C::c3 || configuration == C::c4;
    auto during = core::detail::json_object<Json>([&](auto& value) {
        core::detail::set_json(value, "enabled", Json(scheduled));
        if (scheduled) core::detail::set_json(value, "interval", Json("100000000"));
    });
    core::detail::JsonCleanup during_cleanup{during};
    auto sampling = core::detail::json_object<Json>([&](auto& value) {
        core::detail::set_json(value, "before_observation", Json(configuration != C::c0));
        core::detail::set_json(value, "after_observation", Json(configuration != C::c0));
        core::detail::set_json(value, "during_observation", std::move(during));
    });
    core::detail::JsonCleanup sampling_cleanup{sampling};
    return core::detail::json_object<Json>([&](auto& value) {
        core::detail::set_json(value, "required", std::move(required));
        core::detail::set_json(value, "optional", Json::array());
        core::detail::set_json(value, "sampling", std::move(sampling));
        core::detail::set_json(value, "unavailable_optional_source", Json("fail"));
    });
}

struct HostFrozenObservationAdmission::State final {
    HostQ0bFamily family;
    HostFrozenConfiguration configuration;
    std::optional<runtime::ipc::PlacementRequest> request;
    std::optional<runtime::AdmittedPlacementRequest> placement;
    std::optional<MonitoringExecutionPolicy> policy;
    std::optional<runtime::ServicePlacementPlan> service;
    std::optional<MonitoringHostAdmittedProductionFacts> original_facts;
    std::optional<core::Sha256Digest> profile_sha256;
    std::optional<HostFrozenWork> work;
    ~State() noexcept {
        placement.reset();
        service.reset();
        if (request)
            runtime::ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(*request);
    }
    State(HostQ0bFamily selected, HostFrozenConfiguration config) noexcept
        : family{selected}, configuration{config} {}
};

HostFrozenAdmissionFailure::HostFrozenAdmissionFailure(HostFrozenAdmissionReason selected,
    std::optional<core::Error> error, HostFrozenBindingCheckpoint checkpoint) noexcept
    : reason{selected}, lower{std::move(error)}, binding_checkpoint{checkpoint} {}
HostFrozenAdmissionFailure::~HostFrozenAdmissionFailure() noexcept {
    if (lower)
        core::detail::retire_json(lower->context);
}

HostFrozenObservationAdmission::HostFrozenObservationAdmission(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
HostFrozenObservationAdmission::HostFrozenObservationAdmission(HostFrozenObservationAdmission&&) noexcept = default;
HostFrozenObservationAdmission::~HostFrozenObservationAdmission() noexcept = default;

auto HostFrozenObservationAdmission::view() const noexcept -> std::optional<HostFrozenObservationView> {
    if (!state_) return {};
    return HostFrozenObservationView{state_->family, state_->configuration, *state_->request,
        *state_->placement, *state_->policy, *state_->service, *state_->original_facts,
        *state_->profile_sha256, *state_->work};
}

auto HostFrozenObservationAdmission::create_fallible(
    const RunnerContextAcquisitionSession& session,
    const HostEnvironmentAuthority* retained_environment,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs,
    const MonitoringQ1ValidatedPublicValues& values, runtime::ProductionRequestIdSource& ids,
    HostQ0bFamily family, HostFrozenConfiguration configuration,
    std::optional<core::Error>* raw_failure)
    -> std::expected<HostFrozenObservationAdmission, HostFrozenAdmissionFailure> {
    if ((family != HostQ0bFamily::bandwidth && family != HostQ0bFamily::latency) ||
        !valid(configuration))
        return std::unexpected{failure(Reason::invalid_selector)};
    if (!inputs.bandwidth().has_value() || !inputs.latency().has_value())
        return std::unexpected{failure(Reason::unavailable_input)};
    const auto workers = plan.bandwidth_workers().size();
    if (workers == 0 || workers > 8 || plan.latency_workers().size() != 1)
        return std::unexpected{failure(Reason::worker_bound)};
    const auto& q0 = values.q0();
    const auto bytes = plan.working_set_bytes().value();
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (bytes == 0 || q0.selected_work[0] == 0 || q0.selected_work[1] == 0 ||
        bytes > maximum / workers || q0.selected_work[0] > maximum / (bytes * workers))
        return std::unexpected{failure(Reason::work_bound)};

    // First allocation is our own State: permanent allocation denial cannot
    // enter a lower noexcept projector's allocating error construction.
    auto state = std::make_unique<State>(family, configuration);
    auto bandwidth =
        HostQ0bProjectionAccess::facts(session, plan, inputs.bandwidth(), HostQ0bFamily::bandwidth,
                                       runtime::PairwiseVariant::first, raw_failure);
    core::detail::ExpectedErrorCleanup bandwidth_cleanup{bandwidth};
    if (!bandwidth)
        return std::unexpected{failure(Reason::original_facts, std::move(bandwidth.error()))};
    auto latency =
        HostQ0bProjectionAccess::facts(session, plan, inputs.latency(), HostQ0bFamily::latency,
                                       runtime::PairwiseVariant::first, raw_failure);
    core::detail::ExpectedErrorCleanup latency_cleanup{latency};
    if (!latency)
        return std::unexpected{failure(Reason::original_facts, std::move(latency.error()))};
    std::optional<HostEnvironmentAuthority> ordinary_environment;
    if (!retained_environment) {
        auto derived = derive_host_environment_fallible(session, raw_failure);
        core::detail::ExpectedErrorCleanup derived_cleanup{derived};
        if (!derived)
            return std::unexpected{failure(Reason::original_facts, std::move(derived.error()))};
        ordinary_environment.emplace(std::move(*derived));
        retained_environment = &*ordinary_environment;
    }
    const auto* environment = retained_environment;
    auto base =
        HostQ0bProjectionAccess::base(*environment, plan, *bandwidth, *latency, raw_failure);
    core::detail::ExpectedErrorCleanup base_cleanup{base};
    if (!base)
        return std::unexpected{failure(Reason::original_facts, std::move(base.error()))};
    auto original_inputs = detail_RunnerContextAcquisitionAccess::input_template_fallible(session);
    core::detail::ExpectedErrorCleanup original_inputs_cleanup{original_inputs};
    if (!original_inputs)
        return std::unexpected{failure(Reason::original_facts, std::move(original_inputs.error()))};
    // Preserve the original short-circuit order without re-evaluating a failed
    // predicate or allocating a diagnostic string after its rejection.
    using Checkpoint = HostFrozenBindingCheckpoint;
    if (q0.environment_schema != "epycbench.monitoring-host-environment/v2")
        return std::unexpected{binding_failure(Checkpoint::environment_schema)};
    if (q0.environment_digest != environment->environment_digest())
        return std::unexpected{binding_failure(Checkpoint::environment_digest)};
    if (q0.qualification_plan_schema != "epycbench.monitoring-host-qualification-plan/v2")
        return std::unexpected{binding_failure(Checkpoint::qualification_plan_schema)};
    if (q0.qualification_plan_digest != plan.plan_digest())
        return std::unexpected{binding_failure(Checkpoint::qualification_plan_digest)};
    if (q0.production_inputs_schema != "epycbench.monitoring-q0b-production-inputs/v2")
        return std::unexpected{binding_failure(Checkpoint::production_inputs_schema)};
    if (q0.production_inputs_sha256 != original_inputs->get().manifest_sha256())
        return std::unexpected{binding_failure(Checkpoint::production_inputs_sha256)};
    if (q0.base_environment_schema != "epycbench.monitoring-q0b-base-environment/v2")
        return std::unexpected{binding_failure(Checkpoint::base_environment_schema)};
    if (q0.base_environment_sha256 != base->digest())
        return std::unexpected{binding_failure(Checkpoint::base_environment_sha256)};
    if (!family_binding(q0.family_admissions[0], *bandwidth, "sequential-read-bandwidth", base->digest()))
        return std::unexpected{binding_failure(Checkpoint::bandwidth_family)};
    if (!family_binding(q0.family_admissions[1], *latency, "dependent-load-latency", base->digest()))
        return std::unexpected{binding_failure(Checkpoint::latency_family)};
    if (values.policy_identity() != original_inputs->get().policy_identity())
        return std::unexpected{binding_failure(Checkpoint::policy_identity)};
    if (values.policy_sha256() != original_inputs->get().policy_sha256().to_hex())
        return std::unexpected{binding_failure(Checkpoint::policy_sha256)};
    auto profile = core::Sha256Digest::parse(values.profile_sha256());
    core::detail::ExpectedErrorCleanup profile_cleanup{profile};
    if (!profile)
        return std::unexpected{failure(Reason::profile_binding, std::move(profile.error()))};
    state->profile_sha256.emplace(*profile);
    if (family == HostQ0bFamily::bandwidth) {
        auto passes = kernel::SequentialReadBandwidthPassCount::create(q0.selected_work[0]);
        core::detail::ExpectedErrorCleanup passes_cleanup{passes};
        if (!passes)
            return std::unexpected{failure(Reason::work_bound, std::move(passes.error()))};
        state->work.emplace(std::in_place_type<kernel::SequentialReadBandwidthPassCount>, *passes);
    } else {
        auto steps = kernel::DependentLoadLatencyStepCount::create(q0.selected_work[1]);
        core::detail::ExpectedErrorCleanup steps_cleanup{steps};
        if (!steps)
            return std::unexpected{failure(Reason::work_bound, std::move(steps.error()))};
        state->work.emplace(std::in_place_type<kernel::DependentLoadLatencyStepCount>, *steps);
    }
    const auto& original =
        family == HostQ0bFamily::bandwidth ? inputs.bandwidth() : inputs.latency();
    auto wire = core::detail::clone_json_as<nlohmann::json>(original.first_request().wire());
    core::detail::JsonCleanup wire_cleanup{wire};
    wire["execution"]["measurement"]["calibration"]["enabled"] = false;
    core::detail::set_json(wire.at("execution"), "monitoring", monitoring_wire(configuration));
    auto request = runtime::ipc::detail::HostPlacementRequestConstructionAccess::parse_fallible(
        wire, raw_failure);
    runtime::ipc::detail::HostPlacementRequestResultCleanup request_cleanup{request};
    if (!request)
        return std::unexpected{failure(Reason::request, std::move(request.error()))};
    state->request.emplace(std::move(*request));
    auto id = runtime::detail::ProductionRequestIdAccess::allocate(ids);
    core::detail::ExpectedErrorCleanup id_cleanup{id};
    if (!id)
        return std::unexpected{failure(Reason::request_id, std::move(id.error()))};
    auto placement =
        runtime::detail::PairwiseV2AdmissionClassificationAccess::admit_placement_source(
            *id, *state->request, original.first().context(), raw_failure);
    core::detail::ExpectedErrorCleanup placement_cleanup{placement};
    if (!placement)
        return std::unexpected{failure(Reason::placement, std::move(placement.error()))};
    if (placement->placement_identity() != original.first().placement_identity())
        return std::unexpected{failure(Reason::placement)};
    state->placement.emplace(std::move(*placement));
    auto policy =
        runtime::detail::project_production_monitoring_policy(*state->request, raw_failure);
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if (!policy)
        return std::unexpected{failure(Reason::monitoring_policy, std::move(policy.error()))};
    const bool measurement = runtime::detail::production_monitoring_needs_service(*policy);
    auto service =
        runtime::detail::PairwiseV2AdmissionClassificationAccess::create_service_placement_source(
            *state->placement, measurement);
    core::detail::ExpectedErrorCleanup service_cleanup{service};
    if (!service)
        return std::unexpected{failure(Reason::service_placement, std::move(service.error()))};
    if (!exact_service(*service, plan, measurement))
        return std::unexpected{failure(Reason::service_placement)};
    state->policy.emplace(std::move(*policy));
    state->service.emplace(std::move(*service));
    state->original_facts.emplace(family == HostQ0bFamily::bandwidth ? std::move(*bandwidth)
                                                                     : std::move(*latency));
    return HostFrozenObservationAdmission{std::move(state)};
}

auto HostFrozenObservationAdmission::create(const RunnerContextAcquisitionSession& session,
                                            const MonitoringHostQualificationPlan& plan,
                                            const HostQualificationCalibrationInputs& inputs,
                                            const MonitoringQ1ValidatedPublicValues& values,
                                            runtime::ProductionRequestIdSource& ids,
                                            HostQ0bFamily family,
                                            HostFrozenConfiguration configuration) noexcept
    -> std::expected<HostFrozenObservationAdmission, HostFrozenAdmissionFailure> {
    try {
        return create_fallible(session, nullptr, plan, inputs, values, ids, family, configuration,
                               nullptr);
    } catch (const std::bad_alloc&) {
        return std::unexpected{failure(Reason::allocation)};
    } catch (...) {
        return std::unexpected{failure(Reason::contained_exception)};
    }
}
auto HostFrozenObservationAdmission::create_source(
    const RunnerContextAcquisitionSession& session, const HostEnvironmentAuthority& environment,
    const MonitoringHostQualificationPlan& plan, const HostQualificationCalibrationInputs& inputs,
    const MonitoringQ1ValidatedPublicValues& values, runtime::ProductionRequestIdSource& ids,
    HostQ0bFamily family, HostFrozenConfiguration configuration,
    std::optional<core::Error>* raw_failure)
    -> std::expected<HostFrozenObservationAdmission, HostFrozenAdmissionFailure> {
    return create_fallible(session, &environment, plan, inputs, values, ids, family, configuration,
                           raw_failure);
}
} // namespace epycbench::monitoring::qualification::detail
