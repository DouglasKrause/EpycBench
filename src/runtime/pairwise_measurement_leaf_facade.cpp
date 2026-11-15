#include "runtime/pairwise_measurement_leaf_facade.hpp"

#include "measurement/measurement_leaf_access.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto measurement_failure(PairwiseFailureCode code,
                                       PairwiseFailureOperation operation,
                                       PairwiseFailureReason reason,
                                       PairwiseFailureContext context) noexcept
    -> PairwiseMeasurementFailure {
    auto result = PairwiseMeasurementFailure::create(
        code, operation, reason, std::move(context), std::nullopt);
    if (!result)
        std::terminate();
    return std::move(*result);
}

template <class Value>
[[nodiscard]] auto failed(PairwiseFailureCode code, PairwiseFailureOperation operation,
                          PairwiseFailureReason reason,
                          PairwiseFailureContext context = NoPairwiseFailureContext{}) noexcept
    -> std::expected<Value, PairwiseMeasurementFailure> {
    return std::unexpected{measurement_failure(code, operation, reason, std::move(context))};
}

template <class State>
[[nodiscard]] auto map_repetition_access(
    std::expected<State, measurement::detail::RepetitionAccessError> result) noexcept
    -> std::expected<State, PairwiseMeasurementFailure> {
    if (result)
        return std::move(*result);
    if (result.error() ==
        measurement::detail::RepetitionAccessError::result_identity_mismatch)
        return failed<State>(
            PairwiseFailureCode::measurement_repetition_result_identity_mismatch,
            PairwiseFailureOperation::measurement_repetition_observation_apply,
            PairwiseFailureReason::result_identity_mismatch);
    return failed<State>(
        PairwiseFailureCode::measurement_repetition_invalid_transition,
        PairwiseFailureOperation::measurement_repetition_transition_apply,
        PairwiseFailureReason::invalid_transition);
}

[[nodiscard]] constexpr auto family_context(PairwiseMeasurementFamily family) noexcept
    -> KernelIdentityContext {
    return family == PairwiseMeasurementFamily::bandwidth
               ? KernelIdentityContext{
                     PairwiseKernelFamily::bandwidth,
                     PairwiseKernelImplementation::scalar_u64_sequential_v1,
                     PairwiseKernelPhase::none}
               : KernelIdentityContext{
                     PairwiseKernelFamily::latency,
                     PairwiseKernelImplementation::scalar_u64_random_cycle_v1,
                     PairwiseKernelPhase::none};
}

} // namespace

auto PairwiseMeasurementLeafFacade::calibration_duration_policy(
    core::Duration minimum, core::Duration target, core::Duration maximum) const noexcept
    -> std::expected<measurement::CalibrationDurationPolicy, PairwiseMeasurementFailure> {
    if (minimum > target || target > maximum)
        return failed<measurement::CalibrationDurationPolicy>(
            PairwiseFailureCode::measurement_calibration_invalid_duration_policy,
            PairwiseFailureOperation::measurement_calibration_duration_policy_create,
            PairwiseFailureReason::duration_order_invalid);
    return measurement::detail::MeasurementLeafAccess::calibration_policy(minimum, target,
                                                                           maximum);
}

auto PairwiseMeasurementLeafFacade::warmup_stability_threshold(
    std::uint64_t numerator, std::uint64_t denominator) const noexcept
    -> std::expected<measurement::WarmupStabilityThreshold,
                     PairwiseMeasurementFailure> {
    if (denominator == 0U)
        return failed<measurement::WarmupStabilityThreshold>(
            PairwiseFailureCode::measurement_warmup_invalid_policy,
            PairwiseFailureOperation::measurement_warmup_policy_create,
            PairwiseFailureReason::policy_invariant_invalid);
    return measurement::detail::MeasurementLeafAccess::warmup_threshold(numerator,
                                                                         denominator);
}

auto PairwiseMeasurementLeafFacade::warmup_policy(
    std::uint64_t minimum_trials, std::uint64_t maximum_trials,
    std::uint64_t stability_window,
    measurement::WarmupStabilityThreshold stability_threshold) const noexcept
    -> std::expected<measurement::WarmupPolicy, PairwiseMeasurementFailure> {
    const auto disabled_invalid = maximum_trials == 0U && minimum_trials != 0U;
    const auto enabled_invalid = maximum_trials > 0U &&
                                 (stability_window == 0U ||
                                  stability_window > maximum_trials);
    if (minimum_trials > maximum_trials || disabled_invalid || enabled_invalid)
        return failed<measurement::WarmupPolicy>(
            PairwiseFailureCode::measurement_warmup_invalid_policy,
            PairwiseFailureOperation::measurement_warmup_policy_create,
            PairwiseFailureReason::policy_invariant_invalid);
    return measurement::detail::MeasurementLeafAccess::warmup_policy(
        minimum_trials, maximum_trials, stability_window, stability_threshold);
}

auto PairwiseMeasurementLeafFacade::measurement_interval(
    platform_linux::RawMonotonicTimestamp start,
    platform_linux::RawMonotonicTimestamp end,
    PairwiseVerificationPhase phase) const noexcept
    -> std::expected<measurement::MeasurementInterval, PairwiseMeasurementFailure> {
    if (end < start)
        return failed<measurement::MeasurementInterval>(
            PairwiseFailureCode::measurement_interval_invalid_order,
            PairwiseFailureOperation::measurement_interval_create,
            PairwiseFailureReason::reversed_observation_interval,
            VerificationPhaseContext{phase});
    return measurement::detail::MeasurementLeafAccess::interval(start, end);
}

auto PairwiseMeasurementLeafFacade::validate_bandwidth_workers(
    std::span<const core::WorkerId> expected,
    std::vector<measurement::SequentialReadBandwidthWorkerObservation> workers) const noexcept
    -> std::expected<PairwiseValidatedBandwidthWorkers, PairwiseMeasurementFailure> {
    constexpr auto operation =
        PairwiseFailureOperation::measurement_observation_bandwidth_execute;
    if (expected.empty())
        return failed<PairwiseValidatedBandwidthWorkers>(
            PairwiseFailureCode::measurement_observation_empty_workers, operation,
            PairwiseFailureReason::empty_workers);
    const auto matches = expected.size() == workers.size() &&
                         std::ranges::equal(
                             expected, workers, {}, std::identity{},
                             [](const auto& worker) { return worker.worker_id(); });
    if (!matches)
        return failed<PairwiseValidatedBandwidthWorkers>(
            PairwiseFailureCode::measurement_observation_worker_evidence_mismatch,
            operation, PairwiseFailureReason::worker_count_mismatch,
            ExpectedObservedCountContext{{static_cast<std::uint64_t>(expected.size())},
                                         {static_cast<std::uint64_t>(workers.size())}});
    return PairwiseValidatedBandwidthWorkers{std::move(workers)};
}

auto PairwiseMeasurementLeafFacade::validate_latency_workers(
    std::span<const core::WorkerId> expected,
    std::vector<measurement::DependentLoadLatencyWorkerObservation> workers) const noexcept
    -> std::expected<PairwiseValidatedLatencyWorkers, PairwiseMeasurementFailure> {
    constexpr auto operation =
        PairwiseFailureOperation::measurement_observation_latency_execute;
    if (expected.empty())
        return failed<PairwiseValidatedLatencyWorkers>(
            PairwiseFailureCode::measurement_observation_empty_workers, operation,
            PairwiseFailureReason::empty_workers);
    const auto matches = expected.size() == workers.size() &&
                         std::ranges::equal(
                             expected, workers, {}, std::identity{},
                             [](const auto& worker) { return worker.worker_id(); });
    if (!matches)
        return failed<PairwiseValidatedLatencyWorkers>(
            PairwiseFailureCode::measurement_observation_worker_evidence_mismatch,
            operation, PairwiseFailureReason::worker_count_mismatch,
            ExpectedObservedCountContext{{static_cast<std::uint64_t>(expected.size())},
                                         {static_cast<std::uint64_t>(workers.size())}});
    return PairwiseValidatedLatencyWorkers{std::move(workers)};
}

auto PairwiseMeasurementLeafFacade::bandwidth_observation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::SequentialReadBandwidthConfig configuration,
    kernel::SequentialReadBandwidthPassCount pass_count,
    PairwiseValidatedBandwidthWorkers workers,
    measurement::MeasurementInterval enclosing_interval) const noexcept
    -> std::expected<measurement::SequentialReadBandwidthObservation,
                     PairwiseMeasurementFailure> {
    constexpr auto operation =
        PairwiseFailureOperation::measurement_observation_bandwidth_create;
    auto consumed_workers = workers.consume();
    if (!consumed_workers)
        return failed<measurement::SequentialReadBandwidthObservation>(
            PairwiseFailureCode::measurement_observation_empty_workers,
            PairwiseFailureOperation::measurement_observation_bandwidth_execute,
            PairwiseFailureReason::empty_workers);
    if (implementation_id.family() != kernel::KernelFamily::bandwidth)
        return failed<measurement::SequentialReadBandwidthObservation>(
            PairwiseFailureCode::measurement_observation_implementation_mismatch,
            operation, PairwiseFailureReason::configuration_identity_mismatch,
            family_context(PairwiseMeasurementFamily::bandwidth));
    auto aggregate = core::ByteCount::from_base_units(0U);
    for (const auto& worker : *consumed_workers) {
        const auto next = aggregate.checked_add(worker.work().logical_bytes_read());
        if (!next)
            return failed<measurement::SequentialReadBandwidthObservation>(
                PairwiseFailureCode::measurement_observation_aggregate_work_overflow,
                operation, PairwiseFailureReason::arithmetic_failed);
        aggregate = *next;
    }
    return measurement::detail::MeasurementLeafAccess::bandwidth_observation(
        observation_id, experiment_id, block_id, implementation_id,
        std::move(configuration), pass_count, std::move(*consumed_workers),
        std::move(enclosing_interval), aggregate);
}

auto PairwiseMeasurementLeafFacade::latency_observation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::DependentLoadLatencyConfig configuration,
    PairwiseValidatedLatencyWorkers workers,
    measurement::MeasurementInterval enclosing_interval) const noexcept
    -> std::expected<measurement::DependentLoadLatencyObservation,
                     PairwiseMeasurementFailure> {
    constexpr auto operation =
        PairwiseFailureOperation::measurement_observation_latency_create;
    auto consumed_workers = workers.consume();
    if (!consumed_workers)
        return failed<measurement::DependentLoadLatencyObservation>(
            PairwiseFailureCode::measurement_observation_empty_workers,
            PairwiseFailureOperation::measurement_observation_latency_execute,
            PairwiseFailureReason::empty_workers);
    if (implementation_id.family() != kernel::KernelFamily::latency)
        return failed<measurement::DependentLoadLatencyObservation>(
            PairwiseFailureCode::measurement_observation_implementation_mismatch,
            operation, PairwiseFailureReason::configuration_identity_mismatch,
            family_context(PairwiseMeasurementFamily::latency));
    auto aggregate = core::WorkCount::from_base_units(0U);
    for (const auto& worker : *consumed_workers) {
        const auto next = aggregate.checked_add(
            core::WorkCount::from_base_units(worker.completed_work().value()));
        if (!next)
            return failed<measurement::DependentLoadLatencyObservation>(
                PairwiseFailureCode::measurement_observation_aggregate_work_overflow,
                operation, PairwiseFailureReason::arithmetic_failed);
        aggregate = *next;
    }
    return measurement::detail::MeasurementLeafAccess::latency_observation(
        observation_id, experiment_id, block_id, implementation_id,
        std::move(configuration), std::move(*consumed_workers), std::move(enclosing_interval),
        aggregate);
}

auto PairwiseMeasurementLeafFacade::maximum_harness_overhead(
    std::uint64_t numerator, std::uint64_t denominator) const noexcept
    -> std::expected<measurement::MaximumHarnessOverhead,
                     PairwiseMeasurementFailure> {
    if (denominator == 0U)
        return failed<measurement::MaximumHarnessOverhead>(
            PairwiseFailureCode::measurement_validity_invalid_policy,
            PairwiseFailureOperation::measurement_validity_maximum_harness_overhead_create,
            PairwiseFailureReason::policy_invariant_invalid);
    return measurement::detail::MeasurementLeafAccess::maximum_overhead(numerator,
                                                                         denominator);
}

namespace {

template <class Workers, class Evidence>
[[nodiscard]] auto association_matches(const Workers& workers,
                                       std::span<const Evidence> evidence) noexcept -> bool {
    if (workers.size() != evidence.size())
        return false;
    for (const auto& item : evidence) {
        const auto worker_count = std::ranges::count_if(
            workers, [&](const auto& worker) { return worker.worker_id() == item.worker_id(); });
        const auto evidence_count = std::ranges::count_if(
            evidence, [&](const auto& candidate) {
                return candidate.worker_id() == item.worker_id();
            });
        if (worker_count != 1 || evidence_count != 1)
            return false;
    }
    return true;
}

[[nodiscard]] auto compare_ratios(std::uint64_t ln, std::uint64_t ld,
                                  std::uint64_t rn, std::uint64_t rd) noexcept -> int {
    bool reversed{};
    for (;;) {
        const auto lq = ln / ld;
        const auto rq = rn / rd;
        if (lq != rq) {
            const auto comparison = lq < rq ? -1 : 1;
            return reversed ? -comparison : comparison;
        }
        const auto lr = ln % ld;
        const auto rr = rn % rd;
        if (lr == 0U || rr == 0U) {
            if (lr == rr)
                return 0;
            const auto comparison = lr == 0U ? -1 : 1;
            return reversed ? -comparison : comparison;
        }
        ln = ld;
        ld = lr;
        rn = rd;
        rd = rr;
        reversed = !reversed;
    }
}

template <class Workers>
[[nodiscard]] auto correctness(const Workers& workers) noexcept
    -> measurement::ObservationValidityCheckOutcome {
    return std::ranges::all_of(workers, [](const auto& worker) {
               return worker.verification().matched();
           })
               ? measurement::ObservationValidityCheckOutcome::pass
               : measurement::ObservationValidityCheckOutcome::fail;
}

template <class Workers>
[[nodiscard]] auto containment(
    const Workers& workers, const measurement::MeasurementInterval& enclosing) noexcept
    -> measurement::ObservationValidityCheckOutcome {
    return std::ranges::all_of(workers, [&](const auto& worker) {
               return enclosing.start() <= worker.interval().start() &&
                      worker.interval().end() <= enclosing.end();
           })
               ? measurement::ObservationValidityCheckOutcome::pass
               : measurement::ObservationValidityCheckOutcome::fail;
}

[[nodiscard]] auto placement_outcome(
    std::span<const measurement::WorkerPlacementValidityEvidence> values) noexcept
    -> measurement::ObservationValidityCheckOutcome {
    const auto has = [&](measurement::WorkerPlacementValidityState state) {
        return std::ranges::any_of(values,
                                   [state](const auto& value) { return value.state() == state; });
    };
    if (has(measurement::WorkerPlacementValidityState::not_qualified))
        return measurement::ObservationValidityCheckOutcome::fail;
    if (has(measurement::WorkerPlacementValidityState::unavailable))
        return measurement::ObservationValidityCheckOutcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& value) {
            return value.state() ==
                   measurement::WorkerPlacementValidityState::not_applicable;
        }))
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    return measurement::ObservationValidityCheckOutcome::pass;
}

[[nodiscard]] auto migration_outcome(
    std::span<const measurement::WorkerMigrationValidityEvidence> values) noexcept
    -> measurement::ObservationValidityCheckOutcome {
    const auto has = [&](measurement::WorkerMigrationValidityState state) {
        return std::ranges::any_of(values,
                                   [state](const auto& value) { return value.state() == state; });
    };
    if (has(measurement::WorkerMigrationValidityState::migrated))
        return measurement::ObservationValidityCheckOutcome::fail;
    if (has(measurement::WorkerMigrationValidityState::unavailable))
        return measurement::ObservationValidityCheckOutcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& value) {
            return value.state() ==
                   measurement::WorkerMigrationValidityState::not_applicable;
        }))
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    return measurement::ObservationValidityCheckOutcome::pass;
}

[[nodiscard]] auto monitoring_outcome(
    measurement::ObservationMonitoringSignalState state,
    bool detected_is_failure) noexcept -> measurement::ObservationValidityCheckOutcome {
    switch (state) {
    case measurement::ObservationMonitoringSignalState::clear:
        return measurement::ObservationValidityCheckOutcome::pass;
    case measurement::ObservationMonitoringSignalState::detected:
        return detected_is_failure ? measurement::ObservationValidityCheckOutcome::fail
                                   : measurement::ObservationValidityCheckOutcome::warning;
    case measurement::ObservationMonitoringSignalState::unavailable:
        return measurement::ObservationValidityCheckOutcome::unavailable;
    case measurement::ObservationMonitoringSignalState::not_applicable:
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    }
    return measurement::ObservationValidityCheckOutcome::unavailable;
}

template <class Observation>
[[nodiscard]] auto evaluate_validity(
    const Observation& observation, measurement::ObservationValidityPolicy policy,
    measurement::TimerOverheadEstimate estimate,
    std::span<const measurement::WorkerPlacementValidityEvidence> placement,
    std::span<const measurement::WorkerMigrationValidityEvidence> migration,
    measurement::ObservationMonitoringEvidence monitoring,
    PairwiseFailureOperation operation) noexcept
    -> std::expected<measurement::ObservationValidityEvidence,
                     PairwiseMeasurementFailure> {
    const auto workers = observation.workers();
    const auto placement_matches = association_matches(workers, placement);
    const auto migration_matches = association_matches(workers, migration);
    if (!placement_matches || !migration_matches)
        return failed<measurement::ObservationValidityEvidence>(
            PairwiseFailureCode::measurement_validity_worker_evidence_mismatch,
            operation, PairwiseFailureReason::worker_count_mismatch,
            ExpectedObservedCountContext{
                {static_cast<std::uint64_t>(workers.size())},
                {static_cast<std::uint64_t>(placement_matches ? migration.size()
                                                              : placement.size())}});

    const auto elapsed = observation.enclosing_interval().elapsed().value();
    const auto threshold = policy.maximum_harness_overhead();
    const auto estimate_ns = estimate.duration().value();
    std::optional<measurement::TimerOverheadFraction> fraction;
    auto timer_outcome = measurement::ObservationValidityCheckOutcome::fail;
    if (elapsed != 0U) {
        fraction.emplace(measurement::TimerOverheadFraction::create(estimate_ns, elapsed));
        timer_outcome =
            compare_ratios(estimate_ns, elapsed, threshold.numerator(),
                           threshold.denominator()) <= 0
                ? measurement::ObservationValidityCheckOutcome::pass
                : measurement::ObservationValidityCheckOutcome::fail;
    }
    auto timer = measurement::TimerOverheadValidityEvidence::create(
        estimate, threshold, fraction);
    measurement::ObservationValidityChecks checks{
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::kernel_correctness,
            correctness(workers)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::worker_interval_containment,
            containment(workers, observation.enclosing_interval())),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::timer_overhead, timer_outcome),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::placement,
            placement_outcome(placement)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::migration,
            migration_outcome(migration)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::major_page_faults,
            monitoring_outcome(monitoring.major_page_faults(), true)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::throttling,
            monitoring_outcome(monitoring.throttling(), true)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::context_switching,
            monitoring_outcome(monitoring.context_switching(), false)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::frequency_variation,
            monitoring_outcome(monitoring.frequency_variation(), false)),
        measurement::ObservationValidityCheck::create(
            measurement::ObservationValidityCheckId::thermal_variation,
            monitoring_outcome(monitoring.thermal_variation(), false))};
    const auto invalid = std::ranges::any_of(checks, [](const auto& check) {
        return check.outcome() == measurement::ObservationValidityCheckOutcome::fail;
    });
    return measurement::ObservationValidityEvidence::create(
        observation.observation_id(),
        invalid ? measurement::ObservationValidityDisposition::invalid
                : measurement::ObservationValidityDisposition::valid,
        std::move(checks), std::move(policy), std::move(timer));
}

} // namespace

auto PairwiseMeasurementLeafFacade::bandwidth_validity(
    const measurement::SequentialReadBandwidthObservation& observation,
    measurement::ObservationValidityPolicy policy,
    measurement::TimerOverheadEstimate estimate,
    std::span<const measurement::WorkerPlacementValidityEvidence> placement,
    std::span<const measurement::WorkerMigrationValidityEvidence> migration,
    measurement::ObservationMonitoringEvidence monitoring) const noexcept
    -> std::expected<measurement::ObservationValidityEvidence,
                     PairwiseMeasurementFailure> {
    return evaluate_validity(
        observation, std::move(policy), estimate, placement, migration, monitoring,
        PairwiseFailureOperation::measurement_validity_bandwidth_evaluate);
}

auto PairwiseMeasurementLeafFacade::latency_validity(
    const measurement::DependentLoadLatencyObservation& observation,
    measurement::ObservationValidityPolicy policy,
    measurement::TimerOverheadEstimate estimate,
    std::span<const measurement::WorkerPlacementValidityEvidence> placement,
    std::span<const measurement::WorkerMigrationValidityEvidence> migration,
    measurement::ObservationMonitoringEvidence monitoring) const noexcept
    -> std::expected<measurement::ObservationValidityEvidence,
                     PairwiseMeasurementFailure> {
    return evaluate_validity(
        observation, std::move(policy), estimate, placement, migration, monitoring,
        PairwiseFailureOperation::measurement_validity_latency_evaluate);
}

auto PairwiseMeasurementLeafFacade::repetition_policy(
    std::uint64_t minimum_blocks, std::uint64_t maximum_blocks,
    std::uint64_t minimum_valid_observations_per_variant,
    bool stopping_enabled) const noexcept
    -> std::expected<measurement::RepetitionPolicy, PairwiseMeasurementFailure> {
    constexpr auto observations_per_block = std::uint64_t{4U};
    const auto largest_safe_maximum =
        std::numeric_limits<std::uint64_t>::max() / observations_per_block + 1U;
    if (minimum_blocks > maximum_blocks || maximum_blocks > largest_safe_maximum)
        return failed<measurement::RepetitionPolicy>(
            PairwiseFailureCode::measurement_repetition_invalid_policy,
            PairwiseFailureOperation::measurement_repetition_policy_create,
            PairwiseFailureReason::invalid_policy);
    return measurement::detail::MeasurementLeafAccess::repetition_policy(
        minimum_blocks, maximum_blocks, minimum_valid_observations_per_variant,
        stopping_enabled);
}

auto PairwiseMeasurementLeafFacade::validate_repetition_transition(
    measurement::RepetitionPhase actual,
    measurement::RepetitionPhase expected) const noexcept
    -> std::expected<void, PairwiseMeasurementFailure> {
    if (actual != expected)
        return failed<void>(
            PairwiseFailureCode::measurement_repetition_invalid_transition,
            PairwiseFailureOperation::measurement_repetition_transition_apply,
            PairwiseFailureReason::invalid_transition);
    return {};
}

auto PairwiseMeasurementLeafFacade::validate_repetition_observation_identity(
    core::ObservationId actual_observation, core::ObservationId expected_observation,
    kernel::DependentLoadLatencyBlockId actual_block,
    kernel::DependentLoadLatencyBlockId expected_block) const noexcept
    -> std::expected<void, PairwiseMeasurementFailure> {
    if (actual_observation != expected_observation || actual_block != expected_block)
        return failed<void>(
            PairwiseFailureCode::measurement_repetition_result_identity_mismatch,
            PairwiseFailureOperation::measurement_repetition_observation_apply,
            PairwiseFailureReason::result_identity_mismatch);
    return {};
}

auto PairwiseMeasurementLeafFacade::create_bandwidth_repetition(
    core::ExperimentId experiment_id, measurement::RepetitionPolicy policy,
    measurement::ComparisonOrderPolicy comparison_order, bool calibration_enabled,
    measurement::WarmupPolicy warmup_policy,
    std::optional<measurement::BandwidthSelectedExecutionState> selected) const noexcept
    -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                     PairwiseMeasurementFailure> {
    return measurement::detail::MeasurementLeafAccess::initialize_repetition<
        measurement::SequentialReadBandwidthRepetitionState>(
        experiment_id, std::move(policy), std::move(comparison_order),
        calibration_enabled, std::move(warmup_policy), std::move(selected));
}

auto PairwiseMeasurementLeafFacade::create_latency_repetition(
    core::ExperimentId experiment_id, measurement::RepetitionPolicy policy,
    measurement::ComparisonOrderPolicy comparison_order, bool calibration_enabled,
    measurement::WarmupPolicy warmup_policy,
    std::optional<measurement::LatencySelectedExecutionState> selected) const noexcept
    -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                     PairwiseMeasurementFailure> {
    return measurement::detail::MeasurementLeafAccess::initialize_repetition<
        measurement::DependentLoadLatencyRepetitionState>(
        experiment_id, std::move(policy), std::move(comparison_order),
        calibration_enabled, std::move(warmup_policy), std::move(selected));
}

auto PairwiseMeasurementLeafFacade::apply_bandwidth_calibration(
    measurement::SequentialReadBandwidthRepetitionState state,
    const measurement::SequentialReadBandwidthCalibrationOutcome& outcome) const noexcept
    -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                     PairwiseMeasurementFailure> {
    std::optional<measurement::BandwidthSelectedExecutionState> selected;
    if (const auto candidate = outcome.selected_candidate())
        selected.emplace(measurement::BandwidthSelectedExecutionState::create(
            *candidate, outcome.timer_overhead_estimate()));
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_calibration(
            std::move(state), std::move(selected)));
}

auto PairwiseMeasurementLeafFacade::apply_latency_calibration(
    measurement::DependentLoadLatencyRepetitionState state,
    const measurement::DependentLoadLatencyCalibrationOutcome& outcome) const noexcept
    -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                     PairwiseMeasurementFailure> {
    std::optional<measurement::LatencySelectedExecutionState> selected;
    if (const auto candidate = outcome.selected_candidate())
        selected.emplace(measurement::LatencySelectedExecutionState::create(
            *candidate, outcome.timer_overhead_estimate()));
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_calibration(
            std::move(state), std::move(selected)));
}

auto PairwiseMeasurementLeafFacade::apply_bandwidth_warmup(
    measurement::SequentialReadBandwidthRepetitionState state,
    const measurement::SequentialReadBandwidthWarmupOutcome&) const noexcept
    -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                     PairwiseMeasurementFailure> {
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_warmup(std::move(state)));
}

auto PairwiseMeasurementLeafFacade::apply_latency_warmup(
    measurement::DependentLoadLatencyRepetitionState state,
    const measurement::DependentLoadLatencyWarmupOutcome&) const noexcept
    -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                     PairwiseMeasurementFailure> {
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_warmup(std::move(state)));
}

auto PairwiseMeasurementLeafFacade::apply_bandwidth_observation(
    measurement::SequentialReadBandwidthRepetitionState state,
    const measurement::SequentialReadBandwidthObservation& observation,
    const measurement::ObservationValidityEvidence& validity) const noexcept
    -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                     PairwiseMeasurementFailure> {
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_observation(
            std::move(state), observation, validity));
}

auto PairwiseMeasurementLeafFacade::apply_latency_observation(
    measurement::DependentLoadLatencyRepetitionState state,
    const measurement::DependentLoadLatencyObservation& observation,
    const measurement::ObservationValidityEvidence& validity) const noexcept
    -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                     PairwiseMeasurementFailure> {
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_observation(
            std::move(state), observation, validity));
}

auto PairwiseMeasurementLeafFacade::apply_bandwidth_precision(
    measurement::SequentialReadBandwidthRepetitionState state,
    measurement::PrecisionStoppingDecision decision) const noexcept
    -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                     PairwiseMeasurementFailure> {
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_precision(
            std::move(state), decision));
}

auto PairwiseMeasurementLeafFacade::apply_latency_precision(
    measurement::DependentLoadLatencyRepetitionState state,
    measurement::PrecisionStoppingDecision decision) const noexcept
    -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                     PairwiseMeasurementFailure> {
    return map_repetition_access(
        measurement::detail::MeasurementLeafAccess::apply_precision(
            std::move(state), decision));
}

auto PairwiseMeasurementLeafFacade::next_bandwidth_action(
    const measurement::SequentialReadBandwidthRepetitionState& state) const noexcept
    -> measurement::SequentialReadBandwidthRepetitionAction {
    return measurement::detail::MeasurementLeafAccess::next_action(state);
}

auto PairwiseMeasurementLeafFacade::next_latency_action(
    const measurement::DependentLoadLatencyRepetitionState& state) const noexcept
    -> measurement::DependentLoadLatencyRepetitionAction {
    return measurement::detail::MeasurementLeafAccess::next_action(state);
}

} // namespace epycbench::runtime::detail
