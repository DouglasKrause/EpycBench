#pragma once

#include "epycbench/measurement/calibration.hpp"
#include "epycbench/measurement/observation.hpp"
#include "epycbench/measurement/repetition.hpp"
#include "epycbench/measurement/validity.hpp"
#include "epycbench/measurement/warmup.hpp"
#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "runtime/pairwise_prepared_typed_measurement_execution.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::runtime::detail {

enum class PairwiseMeasurementFamily : std::uint8_t { bandwidth, latency };

template <class WorkerObservation>
class PairwiseValidatedWorkerObservations final {
  public:
    PairwiseValidatedWorkerObservations(const PairwiseValidatedWorkerObservations&) = delete;
    auto operator=(const PairwiseValidatedWorkerObservations&)
        -> PairwiseValidatedWorkerObservations& = delete;
    PairwiseValidatedWorkerObservations(PairwiseValidatedWorkerObservations&& other) noexcept
        : workers_{std::move(other.workers_)}, available_{other.available_} {
        other.workers_.clear();
        other.available_ = false;
    }
    auto operator=(PairwiseValidatedWorkerObservations&& other) noexcept
        -> PairwiseValidatedWorkerObservations& {
        if (this != &other) {
            workers_ = std::move(other.workers_);
            available_ = other.available_;
            other.workers_.clear();
            other.available_ = false;
        }
        return *this;
    }
    ~PairwiseValidatedWorkerObservations() noexcept = default;

    [[nodiscard]] auto workers() const noexcept -> std::span<const WorkerObservation> {
        return available_ ? std::span<const WorkerObservation>{workers_}
                          : std::span<const WorkerObservation>{};
    }

  private:
    explicit PairwiseValidatedWorkerObservations(
        std::vector<WorkerObservation> workers) noexcept
        : workers_{std::move(workers)} {}
    [[nodiscard]] auto consume() noexcept
        -> std::optional<std::vector<WorkerObservation>> {
        if (!available_)
            return std::nullopt;
        available_ = false;
        return std::move(workers_);
    }
    std::vector<WorkerObservation> workers_;
    bool available_{true};
    friend class PairwiseMeasurementLeafFacade;
};

using PairwiseValidatedBandwidthWorkers = PairwiseValidatedWorkerObservations<
    measurement::SequentialReadBandwidthWorkerObservation>;
using PairwiseValidatedLatencyWorkers = PairwiseValidatedWorkerObservations<
    measurement::DependentLoadLatencyWorkerObservation>;

class PairwiseMeasurementLeafFacade final {
  public:
    [[nodiscard]] auto prepare_bandwidth_execution(
        const measurement::CalibrationDurationPolicy&,
        const measurement::WarmupPolicy&) const noexcept
        -> std::expected<PairwisePreparedBandwidthMeasurementExecution,
                         PairwiseEvidenceFailure>;
    [[nodiscard]] auto prepare_latency_execution(
        const measurement::CalibrationDurationPolicy&,
        const measurement::WarmupPolicy&) const noexcept
        -> std::expected<PairwisePreparedLatencyMeasurementExecution,
                         PairwiseEvidenceFailure>;
    [[nodiscard]] auto calibrate(
        PairwisePreparedBandwidthMeasurementExecution&,
        const measurement::CalibrationDurationPolicy&,
        PairwiseTypedBandwidthCalibrationOperations) const noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::SequentialReadBandwidthCalibrationOutcome>;
    [[nodiscard]] auto calibrate(
        PairwisePreparedLatencyMeasurementExecution&,
        const measurement::CalibrationDurationPolicy&,
        PairwiseTypedLatencyCalibrationOperations) const noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::DependentLoadLatencyCalibrationOutcome>;
    [[nodiscard]] auto warmup(
        PairwisePreparedBandwidthMeasurementExecution&,
        const measurement::WarmupPolicy&,
        kernel::SequentialReadBandwidthPassCount,
        PairwiseTypedBandwidthWarmupOperations) const noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::SequentialReadBandwidthWarmupOutcome>;
    [[nodiscard]] auto warmup(
        PairwisePreparedLatencyMeasurementExecution&,
        const measurement::WarmupPolicy&,
        kernel::DependentLoadLatencyStepCount,
        PairwiseTypedLatencyWarmupOperations) const noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::DependentLoadLatencyWarmupOutcome>;
    [[nodiscard]] auto calibration_duration_policy(
        core::Duration minimum, core::Duration target, core::Duration maximum) const noexcept
        -> std::expected<measurement::CalibrationDurationPolicy, PairwiseMeasurementFailure>;
    [[nodiscard]] auto warmup_stability_threshold(std::uint64_t numerator,
                                                  std::uint64_t denominator) const noexcept
        -> std::expected<measurement::WarmupStabilityThreshold,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto warmup_policy(
        std::uint64_t minimum_trials, std::uint64_t maximum_trials,
        std::uint64_t stability_window,
        measurement::WarmupStabilityThreshold stability_threshold) const noexcept
        -> std::expected<measurement::WarmupPolicy, PairwiseMeasurementFailure>;
    [[nodiscard]] auto measurement_interval(
        platform_linux::RawMonotonicTimestamp start,
        platform_linux::RawMonotonicTimestamp end,
        PairwiseVerificationPhase phase) const noexcept
        -> std::expected<measurement::MeasurementInterval, PairwiseMeasurementFailure>;
    [[nodiscard]] auto validate_bandwidth_workers(
        std::span<const core::WorkerId> expected,
        std::vector<measurement::SequentialReadBandwidthWorkerObservation> workers) const noexcept
        -> std::expected<PairwiseValidatedBandwidthWorkers, PairwiseMeasurementFailure>;
    [[nodiscard]] auto validate_latency_workers(
        std::span<const core::WorkerId> expected,
        std::vector<measurement::DependentLoadLatencyWorkerObservation> workers) const noexcept
        -> std::expected<PairwiseValidatedLatencyWorkers, PairwiseMeasurementFailure>;
    [[nodiscard]] auto bandwidth_observation(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::SequentialReadBandwidthConfig configuration,
        kernel::SequentialReadBandwidthPassCount pass_count,
        PairwiseValidatedBandwidthWorkers workers,
        measurement::MeasurementInterval enclosing_interval) const noexcept
        -> std::expected<measurement::SequentialReadBandwidthObservation,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto latency_observation(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::DependentLoadLatencyConfig configuration,
        PairwiseValidatedLatencyWorkers workers,
        measurement::MeasurementInterval enclosing_interval) const noexcept
        -> std::expected<measurement::DependentLoadLatencyObservation,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto maximum_harness_overhead(std::uint64_t numerator,
                                                std::uint64_t denominator) const noexcept
        -> std::expected<measurement::MaximumHarnessOverhead,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto bandwidth_validity(
        const measurement::SequentialReadBandwidthObservation& observation,
        measurement::ObservationValidityPolicy policy,
        measurement::TimerOverheadEstimate estimate,
        std::span<const measurement::WorkerPlacementValidityEvidence> placement,
        std::span<const measurement::WorkerMigrationValidityEvidence> migration,
        measurement::ObservationMonitoringEvidence monitoring) const noexcept
        -> std::expected<measurement::ObservationValidityEvidence,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto latency_validity(
        const measurement::DependentLoadLatencyObservation& observation,
        measurement::ObservationValidityPolicy policy,
        measurement::TimerOverheadEstimate estimate,
        std::span<const measurement::WorkerPlacementValidityEvidence> placement,
        std::span<const measurement::WorkerMigrationValidityEvidence> migration,
        measurement::ObservationMonitoringEvidence monitoring) const noexcept
        -> std::expected<measurement::ObservationValidityEvidence,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto repetition_policy(
        std::uint64_t minimum_blocks, std::uint64_t maximum_blocks,
        std::uint64_t minimum_valid_observations_per_variant,
        bool stopping_enabled) const noexcept
        -> std::expected<measurement::RepetitionPolicy, PairwiseMeasurementFailure>;
    [[nodiscard]] auto validate_repetition_transition(
        measurement::RepetitionPhase actual,
        measurement::RepetitionPhase expected) const noexcept
        -> std::expected<void, PairwiseMeasurementFailure>;
    [[nodiscard]] auto validate_repetition_observation_identity(
        core::ObservationId actual_observation, core::ObservationId expected_observation,
        kernel::DependentLoadLatencyBlockId actual_block,
        kernel::DependentLoadLatencyBlockId expected_block) const noexcept
        -> std::expected<void, PairwiseMeasurementFailure>;

    [[nodiscard]] auto create_bandwidth_repetition(
        core::ExperimentId experiment_id, measurement::RepetitionPolicy policy,
        measurement::ComparisonOrderPolicy comparison_order, bool calibration_enabled,
        measurement::WarmupPolicy warmup_policy,
        std::optional<measurement::BandwidthSelectedExecutionState> selected = std::nullopt)
        const noexcept
        -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto create_latency_repetition(
        core::ExperimentId experiment_id, measurement::RepetitionPolicy policy,
        measurement::ComparisonOrderPolicy comparison_order, bool calibration_enabled,
        measurement::WarmupPolicy warmup_policy,
        std::optional<measurement::LatencySelectedExecutionState> selected = std::nullopt)
        const noexcept
        -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_bandwidth_calibration(
        measurement::SequentialReadBandwidthRepetitionState state,
        const measurement::SequentialReadBandwidthCalibrationOutcome& outcome) const noexcept
        -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_latency_calibration(
        measurement::DependentLoadLatencyRepetitionState state,
        const measurement::DependentLoadLatencyCalibrationOutcome& outcome) const noexcept
        -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_bandwidth_warmup(
        measurement::SequentialReadBandwidthRepetitionState state,
        const measurement::SequentialReadBandwidthWarmupOutcome& outcome) const noexcept
        -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_latency_warmup(
        measurement::DependentLoadLatencyRepetitionState state,
        const measurement::DependentLoadLatencyWarmupOutcome& outcome) const noexcept
        -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_bandwidth_observation(
        measurement::SequentialReadBandwidthRepetitionState state,
        const measurement::SequentialReadBandwidthObservation& observation,
        const measurement::ObservationValidityEvidence& validity) const noexcept
        -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_latency_observation(
        measurement::DependentLoadLatencyRepetitionState state,
        const measurement::DependentLoadLatencyObservation& observation,
        const measurement::ObservationValidityEvidence& validity) const noexcept
        -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_bandwidth_precision(
        measurement::SequentialReadBandwidthRepetitionState state,
        measurement::PrecisionStoppingDecision decision) const noexcept
        -> std::expected<measurement::SequentialReadBandwidthRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto apply_latency_precision(
        measurement::DependentLoadLatencyRepetitionState state,
        measurement::PrecisionStoppingDecision decision) const noexcept
        -> std::expected<measurement::DependentLoadLatencyRepetitionState,
                         PairwiseMeasurementFailure>;
    [[nodiscard]] auto next_bandwidth_action(
        const measurement::SequentialReadBandwidthRepetitionState& state) const noexcept
        -> measurement::SequentialReadBandwidthRepetitionAction;
    [[nodiscard]] auto next_latency_action(
        const measurement::DependentLoadLatencyRepetitionState& state) const noexcept
        -> measurement::DependentLoadLatencyRepetitionAction;

};

} // namespace epycbench::runtime::detail
