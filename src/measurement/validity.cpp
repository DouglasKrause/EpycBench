#include "epycbench/measurement/validity.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace epycbench::measurement {
namespace {

[[nodiscard]] auto validity_error(std::string code, std::string operation,
                                  std::string classification) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{std::move(code)},
        .message = "Measurement validity evaluation failed",
        .operation = std::move(operation),
        .os_error = std::nullopt,
        .context = {{"classification", std::move(classification)}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

[[nodiscard]] auto invalid_policy_error() -> core::Error {
    return validity_error("measurement.validity.invalid_policy",
                          "measurement.validity.maximum_harness_overhead.create",
                          "zero_denominator");
}

[[nodiscard]] auto worker_evidence_mismatch_error(std::string operation) -> core::Error {
    return validity_error("measurement.validity.worker_evidence_mismatch", std::move(operation),
                          "worker_evidence_mismatch");
}

template <typename Workers, typename Evidence>
[[nodiscard]] auto association_matches(const Workers& workers,
                                       std::span<const Evidence> evidence) noexcept -> bool {
    if (workers.size() != evidence.size()) {
        return false;
    }
    for (const auto& item : evidence) {
        const auto matching_workers = std::ranges::count_if(
            workers, [&](const auto& worker) { return worker.worker_id() == item.worker_id(); });
        const auto matching_evidence = std::ranges::count_if(evidence, [&](const auto& candidate) {
            return candidate.worker_id() == item.worker_id();
        });
        if (matching_workers != 1 || matching_evidence != 1) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] auto compare_nonnegative_ratios(std::uint64_t left_numerator,
                                              std::uint64_t left_denominator,
                                              std::uint64_t right_numerator,
                                              std::uint64_t right_denominator) noexcept -> int {
    auto reversed = false;
    for (;;) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient) {
            const auto comparison = left_quotient < right_quotient ? -1 : 1;
            return reversed ? -comparison : comparison;
        }

        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            if (left_remainder == right_remainder) {
                return 0;
            }
            const auto comparison = left_remainder == 0U ? -1 : 1;
            return reversed ? -comparison : comparison;
        }

        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        reversed = !reversed;
    }
}

template <typename Workers>
[[nodiscard]] auto correctness_outcome(const Workers& workers) noexcept
    -> ObservationValidityCheckOutcome {
    const auto all_matched = std::ranges::all_of(
        workers, [](const auto& worker) { return worker.verification().matched(); });
    return all_matched ? ObservationValidityCheckOutcome::pass
                       : ObservationValidityCheckOutcome::fail;
}

template <typename Workers>
[[nodiscard]] auto containment_outcome(const Workers& workers,
                                       const MeasurementInterval& enclosing) noexcept
    -> ObservationValidityCheckOutcome {
    const auto all_contained = std::ranges::all_of(workers, [&](const auto& worker) {
        return enclosing.start() <= worker.interval().start() &&
               worker.interval().end() <= enclosing.end();
    });
    return all_contained ? ObservationValidityCheckOutcome::pass
                         : ObservationValidityCheckOutcome::fail;
}

[[nodiscard]] auto
placement_outcome(std::span<const WorkerPlacementValidityEvidence> evidence) noexcept
    -> ObservationValidityCheckOutcome {
    const auto has = [&](WorkerPlacementValidityState state) {
        return std::ranges::any_of(evidence,
                                   [state](const auto& item) { return item.state() == state; });
    };
    if (has(WorkerPlacementValidityState::not_qualified)) {
        return ObservationValidityCheckOutcome::fail;
    }
    if (has(WorkerPlacementValidityState::unavailable)) {
        return ObservationValidityCheckOutcome::unavailable;
    }
    if (std::ranges::all_of(evidence, [](const auto& item) {
            return item.state() == WorkerPlacementValidityState::not_applicable;
        })) {
        return ObservationValidityCheckOutcome::not_applicable;
    }
    return ObservationValidityCheckOutcome::pass;
}

[[nodiscard]] auto
migration_outcome(std::span<const WorkerMigrationValidityEvidence> evidence) noexcept
    -> ObservationValidityCheckOutcome {
    const auto has = [&](WorkerMigrationValidityState state) {
        return std::ranges::any_of(evidence,
                                   [state](const auto& item) { return item.state() == state; });
    };
    if (has(WorkerMigrationValidityState::migrated)) {
        return ObservationValidityCheckOutcome::fail;
    }
    if (has(WorkerMigrationValidityState::unavailable)) {
        return ObservationValidityCheckOutcome::unavailable;
    }
    if (std::ranges::all_of(evidence, [](const auto& item) {
            return item.state() == WorkerMigrationValidityState::not_applicable;
        })) {
        return ObservationValidityCheckOutcome::not_applicable;
    }
    return ObservationValidityCheckOutcome::pass;
}

[[nodiscard]] auto monitoring_outcome(ObservationMonitoringSignalState state,
                                      bool detected_is_failure) noexcept
    -> ObservationValidityCheckOutcome {
    switch (state) {
    case ObservationMonitoringSignalState::clear:
        return ObservationValidityCheckOutcome::pass;
    case ObservationMonitoringSignalState::detected:
        return detected_is_failure ? ObservationValidityCheckOutcome::fail
                                   : ObservationValidityCheckOutcome::warning;
    case ObservationMonitoringSignalState::unavailable:
        return ObservationValidityCheckOutcome::unavailable;
    case ObservationMonitoringSignalState::not_applicable:
        return ObservationValidityCheckOutcome::not_applicable;
    }
    return ObservationValidityCheckOutcome::unavailable;
}

struct TimerEvaluation {
    ObservationValidityCheckOutcome outcome;
    TimerOverheadValidityEvidence evidence;
};

[[nodiscard]] auto evaluate_timer(const MeasurementInterval& enclosing,
                                  const ObservationValidityPolicy& policy,
                                  TimerOverheadEstimate estimate) noexcept -> TimerEvaluation {
    const auto elapsed = enclosing.elapsed().value();
    const auto estimate_nanoseconds = estimate.duration().value();
    const auto threshold = policy.maximum_harness_overhead();
    if (elapsed == 0U) {
        return {ObservationValidityCheckOutcome::fail,
                TimerOverheadValidityEvidence::create(estimate, threshold, std::nullopt)};
    }

    auto fraction = TimerOverheadFraction::create(estimate_nanoseconds, elapsed);
    const auto passes =
        compare_nonnegative_ratios(estimate_nanoseconds, elapsed, threshold.numerator(),
                                   threshold.denominator()) <= 0;
    return {passes ? ObservationValidityCheckOutcome::pass : ObservationValidityCheckOutcome::fail,
            TimerOverheadValidityEvidence::create(estimate, threshold, std::move(fraction))};
}

template <typename Observation>
[[nodiscard]] auto
evaluate_validity(const Observation& observation, ObservationValidityPolicy policy,
                  TimerOverheadEstimate estimate,
                  std::span<const WorkerPlacementValidityEvidence> placement,
                  std::span<const WorkerMigrationValidityEvidence> migration,
                  ObservationMonitoringEvidence monitoring, std::string operation)
    -> std::expected<ObservationValidityEvidence, core::Error> {
    const auto workers = observation.workers();
    if (!association_matches(workers, placement) || !association_matches(workers, migration)) {
        return std::unexpected{worker_evidence_mismatch_error(std::move(operation))};
    }

    auto timer = evaluate_timer(observation.enclosing_interval(), policy, estimate);
    ObservationValidityChecks checks{
        ObservationValidityCheck::create(ObservationValidityCheckId::kernel_correctness,
                                         correctness_outcome(workers)),
        ObservationValidityCheck::create(
            ObservationValidityCheckId::worker_interval_containment,
            containment_outcome(workers, observation.enclosing_interval())),
        ObservationValidityCheck::create(ObservationValidityCheckId::timer_overhead, timer.outcome),
        ObservationValidityCheck::create(ObservationValidityCheckId::placement,
                                         placement_outcome(placement)),
        ObservationValidityCheck::create(ObservationValidityCheckId::migration,
                                         migration_outcome(migration)),
        ObservationValidityCheck::create(ObservationValidityCheckId::major_page_faults,
                                         monitoring_outcome(monitoring.major_page_faults(), true)),
        ObservationValidityCheck::create(ObservationValidityCheckId::throttling,
                                         monitoring_outcome(monitoring.throttling(), true)),
        ObservationValidityCheck::create(ObservationValidityCheckId::context_switching,
                                         monitoring_outcome(monitoring.context_switching(), false)),
        ObservationValidityCheck::create(
            ObservationValidityCheckId::frequency_variation,
            monitoring_outcome(monitoring.frequency_variation(), false)),
        ObservationValidityCheck::create(ObservationValidityCheckId::thermal_variation,
                                         monitoring_outcome(monitoring.thermal_variation(), false)),
    };
    const auto invalid = std::ranges::any_of(checks, [](const auto& check) {
        return check.outcome() == ObservationValidityCheckOutcome::fail;
    });
    return ObservationValidityEvidence::create(
        observation.observation_id(),
        invalid ? ObservationValidityDisposition::invalid : ObservationValidityDisposition::valid,
        std::move(checks), std::move(policy), std::move(timer.evidence));
}

} // namespace

MaximumHarnessOverhead::MaximumHarnessOverhead(std::uint64_t numerator,
                                               std::uint64_t denominator) noexcept
    : numerator_{numerator}, denominator_{denominator} {}
auto MaximumHarnessOverhead::create(std::uint64_t numerator, std::uint64_t denominator)
    -> std::expected<MaximumHarnessOverhead, core::Error> {
    if (denominator == 0U) {
        return std::unexpected{invalid_policy_error()};
    }
    return MaximumHarnessOverhead{numerator, denominator};
}
auto MaximumHarnessOverhead::numerator() const noexcept -> std::uint64_t { return numerator_; }
auto MaximumHarnessOverhead::denominator() const noexcept -> std::uint64_t { return denominator_; }

ObservationValidityPolicy::ObservationValidityPolicy(
    MaximumHarnessOverhead maximum_harness_overhead) noexcept
    : maximum_harness_overhead_{std::move(maximum_harness_overhead)} {}
auto ObservationValidityPolicy::create(MaximumHarnessOverhead maximum_harness_overhead) noexcept
    -> ObservationValidityPolicy {
    return ObservationValidityPolicy{std::move(maximum_harness_overhead)};
}
auto ObservationValidityPolicy::maximum_harness_overhead() const noexcept
    -> MaximumHarnessOverhead {
    return maximum_harness_overhead_;
}

TimerOverheadFraction::TimerOverheadFraction(std::uint64_t numerator_nanoseconds,
                                             std::uint64_t denominator_nanoseconds) noexcept
    : numerator_nanoseconds_{numerator_nanoseconds},
      denominator_nanoseconds_{denominator_nanoseconds} {}
auto TimerOverheadFraction::create(std::uint64_t numerator_nanoseconds,
                                   std::uint64_t denominator_nanoseconds) noexcept
    -> TimerOverheadFraction {
    return {numerator_nanoseconds, denominator_nanoseconds};
}
auto TimerOverheadFraction::numerator_nanoseconds() const noexcept -> std::uint64_t {
    return numerator_nanoseconds_;
}
auto TimerOverheadFraction::denominator_nanoseconds() const noexcept -> std::uint64_t {
    return denominator_nanoseconds_;
}

TimerOverheadValidityEvidence::TimerOverheadValidityEvidence(
    TimerOverheadEstimate estimate, MaximumHarnessOverhead threshold,
    std::optional<TimerOverheadFraction> fraction) noexcept
    : estimate_{estimate}, threshold_{std::move(threshold)}, fraction_{std::move(fraction)} {}
auto TimerOverheadValidityEvidence::create(TimerOverheadEstimate estimate,
                                           MaximumHarnessOverhead threshold,
                                           std::optional<TimerOverheadFraction> fraction) noexcept
    -> TimerOverheadValidityEvidence {
    return {estimate, std::move(threshold), std::move(fraction)};
}
auto TimerOverheadValidityEvidence::estimate() const noexcept -> TimerOverheadEstimate {
    return estimate_;
}
auto TimerOverheadValidityEvidence::threshold() const noexcept -> MaximumHarnessOverhead {
    return threshold_;
}
auto TimerOverheadValidityEvidence::fraction() const noexcept
    -> std::optional<TimerOverheadFraction> {
    return fraction_;
}

ObservationValidityCheck::ObservationValidityCheck(ObservationValidityCheckId id,
                                                   ObservationValidityCheckOutcome outcome) noexcept
    : id_{id}, outcome_{outcome} {}
auto ObservationValidityCheck::create(ObservationValidityCheckId id,
                                      ObservationValidityCheckOutcome outcome) noexcept
    -> ObservationValidityCheck {
    return {id, outcome};
}
auto ObservationValidityCheck::id() const noexcept -> ObservationValidityCheckId { return id_; }
auto ObservationValidityCheck::outcome() const noexcept -> ObservationValidityCheckOutcome {
    return outcome_;
}

WorkerPlacementValidityEvidence::WorkerPlacementValidityEvidence(
    core::WorkerId worker_id, WorkerPlacementValidityState state) noexcept
    : worker_id_{worker_id}, state_{state} {}
auto WorkerPlacementValidityEvidence::create(core::WorkerId worker_id,
                                             WorkerPlacementValidityState state) noexcept
    -> WorkerPlacementValidityEvidence {
    return {worker_id, state};
}
auto WorkerPlacementValidityEvidence::worker_id() const noexcept -> core::WorkerId {
    return worker_id_;
}
auto WorkerPlacementValidityEvidence::state() const noexcept -> WorkerPlacementValidityState {
    return state_;
}

WorkerMigrationValidityEvidence::WorkerMigrationValidityEvidence(
    core::WorkerId worker_id, WorkerMigrationValidityState state) noexcept
    : worker_id_{worker_id}, state_{state} {}
auto WorkerMigrationValidityEvidence::create(core::WorkerId worker_id,
                                             WorkerMigrationValidityState state) noexcept
    -> WorkerMigrationValidityEvidence {
    return {worker_id, state};
}
auto WorkerMigrationValidityEvidence::worker_id() const noexcept -> core::WorkerId {
    return worker_id_;
}
auto WorkerMigrationValidityEvidence::state() const noexcept -> WorkerMigrationValidityState {
    return state_;
}

ObservationMonitoringEvidence::ObservationMonitoringEvidence(
    ObservationMonitoringSignalState major_page_faults, ObservationMonitoringSignalState throttling,
    ObservationMonitoringSignalState context_switching,
    ObservationMonitoringSignalState frequency_variation,
    ObservationMonitoringSignalState thermal_variation) noexcept
    : major_page_faults_{major_page_faults}, throttling_{throttling},
      context_switching_{context_switching}, frequency_variation_{frequency_variation},
      thermal_variation_{thermal_variation} {}
auto ObservationMonitoringEvidence::create(
    ObservationMonitoringSignalState major_page_faults, ObservationMonitoringSignalState throttling,
    ObservationMonitoringSignalState context_switching,
    ObservationMonitoringSignalState frequency_variation,
    ObservationMonitoringSignalState thermal_variation) noexcept -> ObservationMonitoringEvidence {
    return {major_page_faults, throttling, context_switching, frequency_variation,
            thermal_variation};
}
auto ObservationMonitoringEvidence::major_page_faults() const noexcept
    -> ObservationMonitoringSignalState {
    return major_page_faults_;
}
auto ObservationMonitoringEvidence::throttling() const noexcept
    -> ObservationMonitoringSignalState {
    return throttling_;
}
auto ObservationMonitoringEvidence::context_switching() const noexcept
    -> ObservationMonitoringSignalState {
    return context_switching_;
}
auto ObservationMonitoringEvidence::frequency_variation() const noexcept
    -> ObservationMonitoringSignalState {
    return frequency_variation_;
}
auto ObservationMonitoringEvidence::thermal_variation() const noexcept
    -> ObservationMonitoringSignalState {
    return thermal_variation_;
}

ObservationValidityEvidence::ObservationValidityEvidence(
    core::ObservationId observation_id, ObservationValidityDisposition disposition,
    ObservationValidityChecks checks, ObservationValidityPolicy policy,
    TimerOverheadValidityEvidence timer_overhead) noexcept
    : observation_id_{observation_id}, disposition_{disposition}, checks_{std::move(checks)},
      policy_{std::move(policy)}, timer_overhead_{std::move(timer_overhead)} {}
auto ObservationValidityEvidence::create(core::ObservationId observation_id,
                                         ObservationValidityDisposition disposition,
                                         ObservationValidityChecks checks,
                                         ObservationValidityPolicy policy,
                                         TimerOverheadValidityEvidence timer_overhead) noexcept
    -> ObservationValidityEvidence {
    return {observation_id, disposition, std::move(checks), std::move(policy),
            std::move(timer_overhead)};
}
auto ObservationValidityEvidence::observation_id() const noexcept -> core::ObservationId {
    return observation_id_;
}
auto ObservationValidityEvidence::disposition() const noexcept -> ObservationValidityDisposition {
    return disposition_;
}
auto ObservationValidityEvidence::checks() const noexcept
    -> std::span<const ObservationValidityCheck> {
    return checks_;
}
auto ObservationValidityEvidence::policy() const noexcept -> const ObservationValidityPolicy& {
    return policy_;
}
auto ObservationValidityEvidence::timer_overhead() const noexcept
    -> const TimerOverheadValidityEvidence& {
    return timer_overhead_;
}

auto evaluate_sequential_read_bandwidth_validity(
    const SequentialReadBandwidthObservation& observation, ObservationValidityPolicy policy,
    TimerOverheadEstimate estimate, std::span<const WorkerPlacementValidityEvidence> placement,
    std::span<const WorkerMigrationValidityEvidence> migration,
    ObservationMonitoringEvidence monitoring)
    -> std::expected<ObservationValidityEvidence, core::Error> {
    return evaluate_validity(observation, std::move(policy), estimate, placement, migration,
                             std::move(monitoring), "measurement.validity.bandwidth.evaluate");
}

auto evaluate_dependent_load_latency_validity(
    const DependentLoadLatencyObservation& observation, ObservationValidityPolicy policy,
    TimerOverheadEstimate estimate, std::span<const WorkerPlacementValidityEvidence> placement,
    std::span<const WorkerMigrationValidityEvidence> migration,
    ObservationMonitoringEvidence monitoring)
    -> std::expected<ObservationValidityEvidence, core::Error> {
    return evaluate_validity(observation, std::move(policy), estimate, placement, migration,
                             std::move(monitoring), "measurement.validity.latency.evaluate");
}

} // namespace epycbench::measurement
