#pragma once

#include "epycbench/runtime/finalized_evidence_error.hpp"
#include "epycbench/measurement/repetition.hpp"
#include "epycbench/memory/region_finalization_diagnostic.hpp"
#include "epycbench/monitoring/acquisition_driver.hpp"
#include "epycbench/runtime/statistics_production_policy.hpp"
#include "epycbench/statistics/adaptive_stopping.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace epycbench::runtime {

struct PairwiseRunProvenance {
    core::ExperimentId experiment_id;
    std::string scenario_profile;
    std::string scenario_digest;
    std::string resolved_plan_digest;
    std::string workload_id;
    std::string first_variant_id;
    std::string second_variant_id;
};

using PairwiseCalibrationEvidence =
    std::variant<measurement::SequentialReadBandwidthCalibrationOutcome,
                 measurement::DependentLoadLatencyCalibrationOutcome>;
using PairwiseWarmupEvidence =
    std::variant<measurement::SequentialReadBandwidthWarmupOutcome,
                 measurement::DependentLoadLatencyWarmupOutcome>;

struct FactualObservationEvidence {
    measurement::ScheduledObservationSlot slot;
    core::ObservationId observation_id;
    measurement::MeasurementObservation observation;
    measurement::ObservationValidityEvidence validity;
    std::optional<monitoring::MonitoringCaptureResult> monitoring;
    std::vector<measurement::WorkerPlacementValidityEvidence> placements{};
    std::vector<measurement::WorkerMigrationValidityEvidence> migrations{};
};

struct FinalizedWorkerEvidence {
    core::WorkerId worker_id;
    memory::RegionFinalizationDiagnostic region;
    std::optional<std::uint64_t> primary_error_ordinal;
    std::optional<std::uint64_t> cleanup_error_ordinal;
};

struct FinalizedVariantEvidence {
    measurement::ComparisonVariantSlot slot;
    std::vector<FinalizedWorkerEvidence> workers;
};

struct PairwiseFinalizationEvidence {
    FinalizedVariantEvidence first;
    FinalizedVariantEvidence second;
};
enum class BlockAttemptCompletion { complete_valid, complete_invalid, operationally_incomplete };

enum class RepetitionBoundaryDisposition { continued, precision_target_reached, maximum_blocks_reached };

struct RunnerStoppingEvaluationEvidence {
    kernel::DependentLoadLatencyBlockId boundary_block_id;
    std::uint64_t completed_blocks;
    std::uint64_t complete_valid_blocks;
    std::uint64_t valid_first_observations;
    std::uint64_t valid_second_observations;
    statistics::BlockBootstrapResult bootstrap;
    statistics::AdaptiveStoppingEvaluation evaluation;
    measurement::PrecisionStoppingDecision decision;
    RepetitionBoundaryDisposition subsequent_disposition;
};

struct NormalRunTerminalEvidence {
    measurement::RepetitionOutcome outcome;
};
struct FailedObservationEvidence final {
    measurement::ScheduledObservationSlot slot;
    core::ObservationId observation_id;
    FinalizedEvidenceError primary_error;
    std::vector<FinalizedEvidenceError> worker_errors;
};

struct PairwiseBlockAttemptEvidence final {
    measurement::MeasurementBlockPlan plan;
    std::vector<FactualObservationEvidence> observations;
    std::optional<FailedObservationEvidence> failed_observation;
    BlockAttemptCompletion completion;
};

struct OperationalRunTerminalEvidence final {
    FinalizedEvidenceError primary_error;
    std::vector<FinalizedEvidenceError> secondary_errors;
    std::optional<kernel::DependentLoadLatencyBlockId> active_block_id;
};

using PairwiseRunTerminalEvidence =
    std::variant<NormalRunTerminalEvidence,
                 OperationalRunTerminalEvidence>;

class FinalizedPairwiseRunEvidence;

namespace detail {
class PreparedPairwiseLedgerStorage;
class PairwiseLedgerPreparedAccess;
class FinalizedEvidencePreparedAccess;
} // namespace detail

class PairwiseRunEvidenceLedger final {
  public:
    [[nodiscard]] static auto create(
        PairwiseRunProvenance provenance,
        StatisticsProductionPolicy statistics_policy,
        monitoring::MonitoringExecutionPolicy monitoring_policy,
        measurement::RepetitionPolicy repetition_policy,
        measurement::ComparisonOrderPolicy comparison_order,
        std::size_t maximum_blocks) noexcept
        -> std::expected<PairwiseRunEvidenceLedger, core::Error>;

    PairwiseRunEvidenceLedger(
        PairwiseRunEvidenceLedger&&) noexcept = default;
    PairwiseRunEvidenceLedger(
        const PairwiseRunEvidenceLedger&) = delete;
    auto operator=(PairwiseRunEvidenceLedger&&)
        -> PairwiseRunEvidenceLedger& = delete;
    auto operator=(const PairwiseRunEvidenceLedger&)
        -> PairwiseRunEvidenceLedger& = delete;

    [[nodiscard]] auto associate_calibration(
        PairwiseCalibrationEvidence&& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto associate_warmup(
        PairwiseWarmupEvidence&& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto calibration_evidence() const noexcept
        -> const PairwiseCalibrationEvidence& {
        return *calibration_;
    }
    [[nodiscard]] auto warmup_evidence() const noexcept
        -> const PairwiseWarmupEvidence& {
        return *warmup_;
    }
    [[nodiscard]] auto append_block(
        PairwiseBlockAttemptEvidence evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto append_stopping_evaluation(
        RunnerStoppingEvaluationEvidence evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto record_normal_terminal(
        NormalRunTerminalEvidence evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto record_operational_terminal(
        OperationalRunTerminalEvidence evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto associate_finalization(
        PairwiseFinalizationEvidence evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto finalize() && noexcept
        -> std::expected<FinalizedPairwiseRunEvidence,
                         PairwiseRuntimeFailure>;

  private:
    // Shared commit boundary: validate before moving the retained input.
    [[nodiscard]] auto append_block_pending(PairwiseBlockAttemptEvidence&) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto append_stopping_evaluation_pending(RunnerStoppingEvaluationEvidence&) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto record_normal_terminal_pending(NormalRunTerminalEvidence&) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto record_operational_terminal_pending(OperationalRunTerminalEvidence&) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    [[nodiscard]] auto associate_finalization_pending(PairwiseFinalizationEvidence&) noexcept
        -> std::expected<void, PairwiseRuntimeFailure>;
    PairwiseRunEvidenceLedger(
        PairwiseRunProvenance,
        StatisticsProductionPolicy,
        monitoring::MonitoringExecutionPolicy,
        measurement::RepetitionPolicy,
        measurement::ComparisonOrderPolicy,
        std::vector<PairwiseBlockAttemptEvidence>,
        std::vector<RunnerStoppingEvaluationEvidence>) noexcept;

    PairwiseRunProvenance provenance_;
    StatisticsProductionPolicy statistics_policy_;
    monitoring::MonitoringExecutionPolicy monitoring_policy_;
    measurement::RepetitionPolicy repetition_policy_;
    measurement::ComparisonOrderPolicy comparison_order_;
    std::optional<PairwiseCalibrationEvidence> calibration_;
    std::optional<PairwiseWarmupEvidence> warmup_;
    std::vector<PairwiseBlockAttemptEvidence> blocks_;
    std::vector<RunnerStoppingEvaluationEvidence> stopping_;
    std::optional<PairwiseRunTerminalEvidence> terminal_;
    std::optional<PairwiseFinalizationEvidence> finalization_;
    bool finalized_{};
    friend class detail::PairwiseLedgerPreparedAccess;
};

namespace detail {

class PreparedPairwiseLedgerStorage final {
  public:
    PreparedPairwiseLedgerStorage(const PreparedPairwiseLedgerStorage&) = delete;
    PreparedPairwiseLedgerStorage(PreparedPairwiseLedgerStorage&&) noexcept = default;
    auto operator=(const PreparedPairwiseLedgerStorage&)
        -> PreparedPairwiseLedgerStorage& = delete;
    auto operator=(PreparedPairwiseLedgerStorage&&)
        -> PreparedPairwiseLedgerStorage& = delete;
    [[nodiscard]] static auto prepare(std::size_t maximum_blocks) noexcept
        -> std::optional<PreparedPairwiseLedgerStorage>;

  private:
    PreparedPairwiseLedgerStorage(std::vector<PairwiseBlockAttemptEvidence> blocks,
                                  std::vector<RunnerStoppingEvaluationEvidence> stopping) noexcept
        : blocks_{std::move(blocks)}, stopping_{std::move(stopping)} {}
    std::vector<PairwiseBlockAttemptEvidence> blocks_;
    std::vector<RunnerStoppingEvaluationEvidence> stopping_;
    friend class PairwiseLedgerPreparedAccess;
};

class PairwiseLedgerPreparedAccess final {
  public:
    // Prepared execution retains every rejected input; legacy by-value APIs
    // delegate to these same validations and keep their consuming semantics.
    [[nodiscard]] static auto append_block(
        PairwiseRunEvidenceLedger& ledger, PairwiseBlockAttemptEvidence& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure> {
        return ledger.append_block_pending(evidence);
    }
    [[nodiscard]] static auto append_stopping_evaluation(
        PairwiseRunEvidenceLedger& ledger, RunnerStoppingEvaluationEvidence& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure> {
        return ledger.append_stopping_evaluation_pending(evidence);
    }
    [[nodiscard]] static auto record_normal_terminal(
        PairwiseRunEvidenceLedger& ledger, NormalRunTerminalEvidence& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure> {
        return ledger.record_normal_terminal_pending(evidence);
    }
    [[nodiscard]] static auto record_operational_terminal(
        PairwiseRunEvidenceLedger& ledger, OperationalRunTerminalEvidence& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure> {
        return ledger.record_operational_terminal_pending(evidence);
    }
    [[nodiscard]] static auto associate_finalization(
        PairwiseRunEvidenceLedger& ledger, PairwiseFinalizationEvidence& evidence) noexcept
        -> std::expected<void, PairwiseRuntimeFailure> {
        return ledger.associate_finalization_pending(evidence);
    }
    [[nodiscard]] static auto create(
        PairwiseRunProvenance provenance,
        StatisticsProductionPolicy statistics_policy,
        monitoring::MonitoringExecutionPolicy monitoring_policy,
        measurement::RepetitionPolicy repetition_policy,
        measurement::ComparisonOrderPolicy comparison_order,
        PreparedPairwiseLedgerStorage storage) noexcept
        -> std::optional<PairwiseRunEvidenceLedger>;
    [[nodiscard]] static auto blocks(const PairwiseRunEvidenceLedger& ledger) noexcept
        -> std::span<const PairwiseBlockAttemptEvidence> {
        return ledger.blocks_;
    }
    [[nodiscard]] static auto statistics_policy(
        const PairwiseRunEvidenceLedger& ledger) noexcept
        -> const StatisticsProductionPolicy& {
        return ledger.statistics_policy_;
    }
    [[nodiscard]] static auto repetition_policy(
        const PairwiseRunEvidenceLedger& ledger) noexcept
        -> const measurement::RepetitionPolicy& {
        return ledger.repetition_policy_;
    }
    [[nodiscard]] static auto experiment_id(
        const PairwiseRunEvidenceLedger& ledger) noexcept -> core::ExperimentId {
        return ledger.provenance_.experiment_id;
    }
    // Prepared reconstruction parses the complete block array before the
    // terminal-owned stopping history. Admit an already-validated historical
    // record without weakening the live ledger's current-boundary rule.
    [[nodiscard]] static auto append_reconstructed_stopping_evaluation(
        PairwiseRunEvidenceLedger& ledger,
        RunnerStoppingEvaluationEvidence evidence) noexcept -> bool;
};

} // namespace detail

class FinalizedPairwiseRunEvidence final {
  public:
    FinalizedPairwiseRunEvidence(
        FinalizedPairwiseRunEvidence&&) noexcept = default;
    FinalizedPairwiseRunEvidence(
        const FinalizedPairwiseRunEvidence&) = delete;
    auto operator=(FinalizedPairwiseRunEvidence&&)
        -> FinalizedPairwiseRunEvidence& = delete;
    auto operator=(const FinalizedPairwiseRunEvidence&)
        -> FinalizedPairwiseRunEvidence& = delete;

    [[nodiscard]] auto provenance() const noexcept
        -> const PairwiseRunProvenance& { return provenance_; }
    [[nodiscard]] auto statistics_policy() const noexcept
        -> const StatisticsProductionPolicy& { return statistics_policy_; }
    [[nodiscard]] auto monitoring_policy() const noexcept
        -> const monitoring::MonitoringExecutionPolicy& {
        return monitoring_policy_;
    }
    [[nodiscard]] auto repetition_policy() const noexcept
        -> const measurement::RepetitionPolicy& { return repetition_policy_; }
    [[nodiscard]] auto comparison_order() const noexcept
        -> const measurement::ComparisonOrderPolicy& {
        return comparison_order_;
    }
    [[nodiscard]] auto calibration() const noexcept
        -> const std::optional<PairwiseCalibrationEvidence>& {
        return calibration_;
    }
    [[nodiscard]] auto warmup() const noexcept
        -> const std::optional<PairwiseWarmupEvidence>& { return warmup_; }
    [[nodiscard]] auto blocks() const noexcept
        -> std::span<const PairwiseBlockAttemptEvidence> { return blocks_; }
    [[nodiscard]] auto stopping_evaluations() const noexcept
        -> std::span<const RunnerStoppingEvaluationEvidence> { return stopping_; }
    [[nodiscard]] auto terminal() const noexcept
        -> const PairwiseRunTerminalEvidence& { return terminal_; }
    [[nodiscard]] auto finalization() const noexcept
        -> const std::optional<PairwiseFinalizationEvidence>& {
        return finalization_;
    }

  private:
    FinalizedPairwiseRunEvidence(
        PairwiseRunProvenance provenance,
        StatisticsProductionPolicy statistics_policy,
        monitoring::MonitoringExecutionPolicy monitoring_policy,
        measurement::RepetitionPolicy repetition_policy,
        measurement::ComparisonOrderPolicy comparison_order,
        std::optional<PairwiseCalibrationEvidence> calibration,
        std::optional<PairwiseWarmupEvidence> warmup,
        std::vector<PairwiseBlockAttemptEvidence> blocks,
        std::vector<RunnerStoppingEvaluationEvidence> stopping,
        PairwiseRunTerminalEvidence terminal,
        std::optional<PairwiseFinalizationEvidence> finalization) noexcept
        : provenance_{std::move(provenance)},
          statistics_policy_{std::move(statistics_policy)},
          monitoring_policy_{std::move(monitoring_policy)},
          repetition_policy_{std::move(repetition_policy)},
          comparison_order_{std::move(comparison_order)},
          calibration_{std::move(calibration)}, warmup_{std::move(warmup)},
          blocks_{std::move(blocks)}, stopping_{std::move(stopping)},
          terminal_{std::move(terminal)},
          finalization_{std::move(finalization)} {}

    PairwiseRunProvenance provenance_;
    StatisticsProductionPolicy statistics_policy_;
    monitoring::MonitoringExecutionPolicy monitoring_policy_;
    measurement::RepetitionPolicy repetition_policy_;
    measurement::ComparisonOrderPolicy comparison_order_;
    std::optional<PairwiseCalibrationEvidence> calibration_;
    std::optional<PairwiseWarmupEvidence> warmup_;
    std::vector<PairwiseBlockAttemptEvidence> blocks_;
    std::vector<RunnerStoppingEvaluationEvidence> stopping_;
    PairwiseRunTerminalEvidence terminal_;
    std::optional<PairwiseFinalizationEvidence> finalization_;
    friend class PairwiseRunEvidenceLedger;
    friend class detail::FinalizedEvidencePreparedAccess;
};

namespace detail {

struct FinalizedEvidenceComponents final {
    PairwiseRunProvenance provenance;
    StatisticsProductionPolicy statistics_policy;
    monitoring::MonitoringExecutionPolicy monitoring_policy;
    measurement::RepetitionPolicy repetition_policy;
    measurement::ComparisonOrderPolicy comparison_order;
    std::optional<PairwiseCalibrationEvidence> calibration;
    std::optional<PairwiseWarmupEvidence> warmup;
    std::vector<PairwiseBlockAttemptEvidence> blocks;
    std::vector<RunnerStoppingEvaluationEvidence> stopping;
    PairwiseRunTerminalEvidence terminal;
    std::optional<PairwiseFinalizationEvidence> finalization;
};

class FinalizedEvidencePreparedAccess final {
  public:
    [[nodiscard]] static auto take(FinalizedPairwiseRunEvidence&& value) noexcept
        -> FinalizedEvidenceComponents {
        return {std::move(value.provenance_), std::move(value.statistics_policy_),
                std::move(value.monitoring_policy_), std::move(value.repetition_policy_),
                std::move(value.comparison_order_), std::move(value.calibration_),
                std::move(value.warmup_), std::move(value.blocks_), std::move(value.stopping_),
                std::move(value.terminal_), std::move(value.finalization_)};
    }
};

} // namespace detail

} // namespace epycbench::runtime
