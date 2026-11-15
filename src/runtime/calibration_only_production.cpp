#include "epycbench/runtime/calibration_only_production.hpp"

#include "core/json_ownership.hpp"
#include "runtime/calibration_only_production_admission_access.hpp"
#include "runtime/ipc/placement_request_construction_access.hpp"
#include "runtime/pairwise_v2_contextual_admission_classification.hpp"
#include "runtime/placement_provenance_access.hpp"
#include "runtime/production_admission_policy.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <optional>
#include <ranges>
#include <utility>

namespace epycbench::runtime {
namespace {

auto incompatible(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code =
                core::StableErrorCode{"runtime.calibration_only_admission.incompatible_requests"},
            .message = "Calibration-only production requests are not compatible",
            .operation = "calibration_only_admission",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json(std::move(classification)));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto contained(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code =
                core::StableErrorCode{
                    "runtime.calibration_only_production_admission.contained_failure"},
            .message = "Calibration-only production admission construction failed",
            .operation = "construct_calibration_only_production_admission",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json(std::move(classification)));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <typename T, typename U>
auto equal_ranges(std::span<const T> first, std::span<const U> second) -> bool {
    return std::ranges::equal(first, second);
}

auto fixed_region_equal(const ipc::PlacementRegionRequest& first,
                        const ipc::PlacementRegionRequest& second) -> bool {
    return first.bytes() == second.bytes() && first.page_mode() == second.page_mode() &&
           first.page_size_bytes() == second.page_size_bytes() &&
           first.placement_threshold() == second.placement_threshold() &&
           first.prefault() == second.prefault() &&
           first.verify_before() == second.verify_before() &&
           first.verify_after() == second.verify_after();
}

auto compatible(const AdmittedPlacementRequest& first,
                const AdmittedPlacementRequest& second) -> std::expected<void, core::Error> {
    if (&first.context() != &second.context()) {
        detail::CalibrationOnlyCompatibilityFacts context_facts{.context_mismatch=true};
        return detail::classify_calibration_only_compatibility(context_facts);
    }
    auto first_scenario=first.request().scenario_provenance();
    if(!first_scenario) return std::unexpected{first_scenario.error()};
    auto second_scenario=second.request().scenario_provenance();
    if(!second_scenario) return std::unexpected{second_scenario.error()};
    detail::CalibrationOnlyCompatibilityFacts facts{
        .context_mismatch = false,
        .scenario_identity_mismatch =
            first_scenario->get().scenario_digest() != second_scenario->get().scenario_digest(),
        .resolved_plan_mismatch = first_scenario->get().resolved_scenario_plan_digest() !=
                                  second_scenario->get().resolved_scenario_plan_digest(),
        .topology_request_mismatch = first.request().topology() != second.request().topology(),
        .experiment_mismatch = first.request().experiment() != second.request().experiment(),
        .kernel_execution_inputs_mismatch =
            first.execution_inputs().kernel() != second.execution_inputs().kernel(),
        .measurement_inputs_mismatch =
            first.execution_inputs().measurement() != second.execution_inputs().measurement(),
        .worker_count_mismatch = first.workers().size() != second.workers().size()};
    if (!facts.worker_count_mismatch) {
    for (std::size_t index = 0; index < first.workers().size(); ++index) {
        const auto& left = first.workers()[index];
        const auto& right = second.workers()[index];
        if (left.processing_unit() != right.processing_unit() || left.core() != right.core() ||
            left.request().cpu() != right.request().cpu()) {
            facts.worker_identity_mismatch = true;
            break;
        }
    }
    for (std::size_t index = 0; index < first.workers().size(); ++index)
        if (!fixed_region_equal(first.workers()[index].request().region(),
                                second.workers()[index].request().region())) {
            facts.worker_region_invariant_mismatch = true;
            break;
        }
    }
    facts.service_reservation_mismatch =
        first.request().reserved_service() != second.request().reserved_service() ||
        !equal_ranges(first.service().cores(), second.service().cores()) ||
        !equal_ranges(first.service().effective_processing_units(),
                      second.service().effective_processing_units()) ||
        !equal_ranges(first.service().linux_cpus(), second.service().linux_cpus());
    return detail::classify_calibration_only_compatibility(facts);
}

auto invalid_operations() -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.calibration_only_operations.invalid"},
            .message = "Calibration-only production operations are invalid",
            .operation = "create_calibration_only_operations",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json("missing_prepare_run"));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

} // namespace

auto detail::classify_calibration_only_compatibility(
    const CalibrationOnlyCompatibilityFacts& facts) noexcept
    -> std::expected<void, core::Error> {
    return classify_calibration_only_compatibility_fallible(facts);
}
auto detail::classify_calibration_only_compatibility_fallible(
    const CalibrationOnlyCompatibilityFacts& facts) -> std::expected<void, core::Error> {
    const std::array ordered{
        std::pair{facts.context_mismatch, "context_mismatch"},
        std::pair{facts.scenario_identity_mismatch, "scenario_identity_mismatch"},
        std::pair{facts.resolved_plan_mismatch, "resolved_plan_mismatch"},
        std::pair{facts.topology_request_mismatch, "topology_request_mismatch"},
        std::pair{facts.experiment_mismatch, "experiment_mismatch"},
        std::pair{facts.kernel_execution_inputs_mismatch, "kernel_execution_inputs_mismatch"},
        std::pair{facts.measurement_inputs_mismatch, "measurement_inputs_mismatch"},
        std::pair{facts.worker_count_mismatch, "worker_count_mismatch"},
        std::pair{facts.worker_identity_mismatch, "worker_identity_mismatch"},
        std::pair{facts.worker_region_invariant_mismatch,
                  "worker_region_invariant_mismatch"},
        std::pair{facts.service_reservation_mismatch, "service_reservation_mismatch"}};
    for (const auto& [failed, classification] : ordered)
        if (failed) return std::unexpected{incompatible(classification)};
    return {};
}

struct CalibrationOnlyProductionAdmission::State final {
    State(ipc::PlacementRequest first_request, ipc::PlacementRequest second_request,
          std::uint64_t seed)
        : first_wire{std::move(first_request)}, second_wire{std::move(second_request)},
          resolved_seed{seed} {}

    ~State() noexcept {
        first_value.reset();
        second_value.reset();
        ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(first_wire);
        ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(second_wire);
    }
    ipc::PlacementRequest first_wire;
    ipc::PlacementRequest second_wire;
    std::uint64_t resolved_seed;
    // Admitted requests are destroyed before the wire requests they borrow.
    std::optional<monitoring::MonitoringExecutionPolicy> monitoring;
    std::optional<ServicePlacementPlan> service;
    std::optional<measurement::CalibrationDurationPolicy> calibration_duration;
    std::optional<AdmittedPlacementRequest> first_value;
    std::optional<AdmittedPlacementRequest> second_value;
};

CalibrationOnlyProductionAdmission::CalibrationOnlyProductionAdmission(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
CalibrationOnlyProductionAdmission::CalibrationOnlyProductionAdmission(
    CalibrationOnlyProductionAdmission&&) noexcept = default;
CalibrationOnlyProductionAdmission::~CalibrationOnlyProductionAdmission() = default;
auto CalibrationOnlyProductionAdmission::first_request() const noexcept
    -> const ipc::PlacementRequest& { assert(state_); return state_->first_wire; }
auto CalibrationOnlyProductionAdmission::second_request() const noexcept
    -> const ipc::PlacementRequest& { assert(state_); return state_->second_wire; }
auto CalibrationOnlyProductionAdmission::first() const noexcept
    -> const AdmittedPlacementRequest& { assert(state_ && state_->first_value); return *state_->first_value; }
auto CalibrationOnlyProductionAdmission::second() const noexcept
    -> const AdmittedPlacementRequest& { assert(state_ && state_->second_value); return *state_->second_value; }
auto CalibrationOnlyProductionAdmission::resolved_comparison_order_seed() const noexcept
    -> std::uint64_t { assert(state_); return state_->resolved_seed; }
auto CalibrationOnlyProductionAdmission::monitoring_policy() const noexcept
    -> const monitoring::MonitoringExecutionPolicy& { assert(state_ && state_->monitoring); return *state_->monitoring; }
auto CalibrationOnlyProductionAdmission::service_placement() const noexcept
    -> const ServicePlacementPlan& { assert(state_ && state_->service); return *state_->service; }
auto CalibrationOnlyProductionAdmission::calibration_policy() const noexcept
    -> const measurement::CalibrationDurationPolicy& {
    assert(state_ && state_->calibration_duration); return *state_->calibration_duration;
}
auto CalibrationOnlyProductionAdmission::has_value() const noexcept -> bool {
    return state_ != nullptr;
}

auto detail::CalibrationOnlyProductionAdmissionAccess::create(
    ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
    ipc::RequestId second_id, std::uint64_t seed, const RunnerContext& context) noexcept
    -> std::expected<CalibrationOnlyProductionAdmission, core::Error> {
    try {
        auto state = std::make_unique<CalibrationOnlyProductionAdmission::State>(
            std::move(first), std::move(second), seed);
        auto first_admitted = admit_placement_request(first_id, state->first_wire, context);
        if (!first_admitted) return std::unexpected{first_admitted.error()};
        auto second_admitted = admit_placement_request(second_id, state->second_wire, context);
        if (!second_admitted) return std::unexpected{second_admitted.error()};
        auto compatibility = compatible(*first_admitted, *second_admitted);
        if (!compatibility) return std::unexpected{compatibility.error()};
        auto first_policy = detail::project_production_monitoring_policy(state->first_wire);
        if (!first_policy) return std::unexpected{first_policy.error()};
        auto second_policy = detail::project_production_monitoring_policy(state->second_wire);
        if (!second_policy) return std::unexpected{second_policy.error()};
        if (*first_policy != *second_policy)
            return std::unexpected{incompatible("monitoring_policy_mismatch")};
        auto placement = ServicePlacementPlan::create(
            *first_admitted, detail::production_monitoring_needs_service(*first_policy));
        if (!placement) return std::unexpected{placement.error()};
        const auto& measurement = first_admitted->execution_inputs().measurement();
        auto calibration = measurement::CalibrationDurationPolicy::create(
            measurement.observation_minimum_duration(),
            measurement.observation_target_duration(),
            measurement.observation_maximum_duration());
        if (!calibration) return std::unexpected{calibration.error()};
        state->monitoring.emplace(std::move(*first_policy));
        state->service.emplace(std::move(*placement));
        state->calibration_duration.emplace(std::move(*calibration));
        state->first_value.emplace(std::move(*first_admitted));
        state->second_value.emplace(std::move(*second_admitted));
        return CalibrationOnlyProductionAdmission{std::move(state)};
    } catch (const std::exception&) {
        return std::unexpected{contained("contained_exception")};
    } catch (...) {
        return std::unexpected{contained("contained_unknown_failure")};
    }
}

auto detail::HostCalibrationOnlyProductionAdmissionAccess::create(
    ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
    ipc::RequestId second_id, std::uint64_t seed, const RunnerContext& context) noexcept
    -> std::expected<CalibrationOnlyProductionAdmission, core::Error> {
    try {
        return create_fallible(std::move(first), std::move(second), first_id, second_id, seed,
                               context, false);
    } catch (const std::exception&) {
        return std::unexpected{contained("contained_exception")};
    } catch (...) {
        return std::unexpected{contained("contained_unknown_failure")};
    }
}
auto detail::HostCalibrationOnlyProductionAdmissionAccess::create_fallible(
    ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
    ipc::RequestId second_id, std::uint64_t seed, const RunnerContext& context, bool source_route,
    std::optional<core::Error>* raw_failure)
    -> std::expected<CalibrationOnlyProductionAdmission, core::Error> {
    ipc::detail::HostPlacementRequestCleanup first_cleanup{first}, second_cleanup{second};
    auto state = std::make_unique<CalibrationOnlyProductionAdmission::State>(
        std::move(first), std::move(second), seed);
    auto first_admitted =
        source_route ? detail::PairwiseV2AdmissionClassificationAccess::admit_placement_source(
                           first_id, state->first_wire, context, raw_failure)
                     : admit_placement_request(first_id, state->first_wire, context);
    core::detail::ExpectedErrorCleanup first_admitted_cleanup{first_admitted};
    if (!first_admitted)
        return std::unexpected{std::move(first_admitted.error())};
    auto second_admitted =
        source_route ? detail::PairwiseV2AdmissionClassificationAccess::admit_placement_source(
                           second_id, state->second_wire, context, raw_failure)
                     : admit_placement_request(second_id, state->second_wire, context);
    core::detail::ExpectedErrorCleanup second_admitted_cleanup{second_admitted};
    if (!second_admitted)
        return std::unexpected{std::move(second_admitted.error())};
    auto fh = source_route ? detail::host_qualification_plan_provenance_fallible(
                                 first_admitted->request().provenance())
                           : first_admitted->request().host_qualification_plan_provenance();
    core::detail::ExpectedErrorCleanup fh_cleanup{fh};
    if (!fh)
        return std::unexpected{std::move(fh.error())};
    auto sh = source_route ? detail::host_qualification_plan_provenance_fallible(
                                 second_admitted->request().provenance())
                           : second_admitted->request().host_qualification_plan_provenance();
    core::detail::ExpectedErrorCleanup sh_cleanup{sh};
    if (!sh)
        return std::unexpected{std::move(sh.error())};
    const auto mismatch = [&](bool failed,
                              std::string classification) -> std::expected<void, core::Error> {
        if (failed)
            return std::unexpected{incompatible(std::move(classification))};
        return {};
    };
    if (auto result = mismatch(fh->get().policy_identity() != sh->get().policy_identity(),
                               "qualification_policy_identity_mismatch");
        !result)
        return std::unexpected{std::move(result.error())};
    if (auto result = mismatch(fh->get().template_sha256() != sh->get().template_sha256(),
                               "qualification_template_mismatch");
        !result)
        return std::unexpected{std::move(result.error())};
    if (auto result = mismatch(fh->get().environment_digest() != sh->get().environment_digest(),
                               "qualification_environment_mismatch");
        !result)
        return std::unexpected{std::move(result.error())};
    if (auto result = mismatch(fh->get().host_qualification_plan_digest() !=
                                   sh->get().host_qualification_plan_digest(),
                               "qualification_plan_mismatch");
        !result)
        return std::unexpected{std::move(result.error())};
    detail::CalibrationOnlyCompatibilityFacts facts{
        .topology_request_mismatch = state->first_wire.topology() != state->second_wire.topology(),
        .experiment_mismatch = state->first_wire.experiment() != state->second_wire.experiment(),
        .kernel_execution_inputs_mismatch = first_admitted->execution_inputs().kernel() !=
                                            second_admitted->execution_inputs().kernel(),
        .measurement_inputs_mismatch = first_admitted->execution_inputs().measurement() !=
                                       second_admitted->execution_inputs().measurement(),
        .worker_count_mismatch =
            first_admitted->workers().size() != second_admitted->workers().size()};
    if (!facts.worker_count_mismatch) {
        for (std::size_t i = 0; i < first_admitted->workers().size(); ++i) {
            const auto& l = first_admitted->workers()[i];
            const auto& r = second_admitted->workers()[i];
            if(l.processing_unit()!=r.processing_unit()||l.core()!=r.core()||l.request().cpu()!=r.request().cpu()){facts.worker_identity_mismatch=true;break;}
        }
          for(std::size_t i=0;i<first_admitted->workers().size();++i)if(!fixed_region_equal(first_admitted->workers()[i].request().region(),second_admitted->workers()[i].request().region())){facts.worker_region_invariant_mismatch=true;break;}
    }
        facts.service_reservation_mismatch=state->first_wire.reserved_service()!=state->second_wire.reserved_service()||
          !equal_ranges(first_admitted->service().cores(),second_admitted->service().cores())||
          !equal_ranges(first_admitted->service().effective_processing_units(),second_admitted->service().effective_processing_units())||
          !equal_ranges(first_admitted->service().linux_cpus(),second_admitted->service().linux_cpus());
        auto compatibility = detail::classify_calibration_only_compatibility_fallible(facts);
        core::detail::ExpectedErrorCleanup compatibility_cleanup{compatibility};
        if (!compatibility)
            return std::unexpected{std::move(compatibility.error())};
        auto fp = detail::project_production_monitoring_policy(state->first_wire, raw_failure);
        core::detail::ExpectedErrorCleanup fp_cleanup{fp};
        if (!fp)
            return std::unexpected{std::move(fp.error())};
        auto sp = detail::project_production_monitoring_policy(state->second_wire, raw_failure);
        core::detail::ExpectedErrorCleanup sp_cleanup{sp};
        if (!sp)
            return std::unexpected{std::move(sp.error())};
        if(*fp!=*sp)return std::unexpected{incompatible("monitoring_policy_mismatch")};
        auto placement =
            source_route
                ? detail::PairwiseV2AdmissionClassificationAccess::create_service_placement_source(
                      *first_admitted, detail::production_monitoring_needs_service(*fp))
                : ServicePlacementPlan::create(*first_admitted,
                                               detail::production_monitoring_needs_service(*fp));
        core::detail::ExpectedErrorCleanup placement_cleanup{placement};
        if (!placement)
            return std::unexpected{std::move(placement.error())};
        const auto&m=first_admitted->execution_inputs().measurement();
        auto calibration = measurement::CalibrationDurationPolicy::create(
            m.observation_minimum_duration(), m.observation_target_duration(),
            m.observation_maximum_duration());
        core::detail::ExpectedErrorCleanup calibration_cleanup{calibration};
        if (!calibration)
            return std::unexpected{std::move(calibration.error())};
        state->monitoring.emplace(std::move(*fp));state->service.emplace(std::move(*placement));state->calibration_duration.emplace(std::move(*calibration));
        state->first_value.emplace(std::move(*first_admitted));state->second_value.emplace(std::move(*second_admitted));
        return CalibrationOnlyProductionAdmission{std::move(state)};
}

CalibrationOnlyProductionRun::CalibrationOnlyProductionRun(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_{std::move(implementation)} {}
CalibrationOnlyProductionRun::CalibrationOnlyProductionRun(
    CalibrationOnlyProductionRun&&) noexcept = default;
CalibrationOnlyProductionRun::~CalibrationOnlyProductionRun() noexcept = default;
auto CalibrationOnlyProductionRun::calibrate_bandwidth(
    const measurement::CalibrationDurationPolicy& policy) noexcept
    -> CalibrationOnlyOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome> {
    return implementation_->calibrate_bandwidth(policy);
}
auto CalibrationOnlyProductionRun::calibrate_latency(
    const measurement::CalibrationDurationPolicy& policy) noexcept
    -> CalibrationOnlyOperationResult<measurement::DependentLoadLatencyCalibrationOutcome> {
    return implementation_->calibrate_latency(policy);
}
auto CalibrationOnlyProductionRun::finalize() && noexcept
    -> CalibrationOnlyOperationResult<void> {
    return implementation_->finalize();
}

CalibrationOnlyProductionOperations::CalibrationOnlyProductionOperations(
    std::shared_ptr<void> context, PrepareFunction prepare) noexcept
    : context_{std::move(context)}, prepare_{prepare} {}
auto CalibrationOnlyProductionOperations::create(std::shared_ptr<void> context,
                                                  PrepareFunction prepare) noexcept
    -> std::expected<CalibrationOnlyProductionOperations, core::Error> {
    if (prepare == nullptr) return std::unexpected{invalid_operations()};
    return CalibrationOnlyProductionOperations{std::move(context), prepare};
}
auto CalibrationOnlyProductionOperations::prepare_run(
    const CalibrationOnlyProductionAdmission& admission) const noexcept
    -> CalibrationOnlyOperationResult<CalibrationOnlyProductionRun> {
    return prepare_(context_.get(), admission);
}

} // namespace epycbench::runtime
