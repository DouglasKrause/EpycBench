#pragma once

#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "epycbench/statistics/outlier_flagging.hpp"

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace epycbench::results {

namespace detail {
class PreparedPairwiseRunResultAccess;
}

enum class PairwiseResultValidity { valid, invalid, not_evaluated };

enum class CalculationStage {
    primary_metric,
    block_representatives,
    paired_effect,
    first_summary,
    second_summary,
    first_outliers,
    second_outliers,
    comparison,
    comparison_bootstrap,
    stopping_comparison,
    stopping_bootstrap,
    stopping_evaluation,
};
enum class StatisticsFailureIdentity {
    primary_metric_unrepresentable,
    block_representative_numeric_failure,
    descriptive_numeric_failure,
    outlier_numeric_failure,
    log_ratio_numeric_failure,
    bootstrap_numeric_failure,
    stopping_numeric_failure,
};
struct FinalCalculationFailure {
    CalculationStage stage;
    StatisticsFailureIdentity identity;
    [[nodiscard]] constexpr auto stable_code() const noexcept -> std::string_view {
        constexpr std::array values{"statistics.primary_metric.unrepresentable",
                                    "statistics.block_representative.numeric_failure",
                                    "statistics.descriptive.numeric_failure",
                                    "statistics.outlier.numeric_failure",
                                    "statistics.log_ratio.numeric_failure",
                                    "statistics.bootstrap.numeric_failure",
                                    "statistics.stopping.numeric_failure"};
        return values[static_cast<std::size_t>(identity)];
    }
    [[nodiscard]] constexpr auto stage_name() const noexcept -> std::string_view {
        constexpr std::array values{"primary_metric",       "block_representatives",
                                    "paired_effect",        "first_summary",
                                    "second_summary",       "first_outliers",
                                    "second_outliers",      "comparison",
                                    "comparison_bootstrap", "stopping_comparison",
                                    "stopping_bootstrap",   "stopping_evaluation"};
        return values[static_cast<std::size_t>(stage)];
    }
    auto operator==(const FinalCalculationFailure&) const -> bool = default;
};

struct RecalculationMismatchDiagnostic {
    std::string code{"results.recalculation_mismatch"};
    kernel::DependentLoadLatencyBlockId boundary_block_id;
    std::string stage;
    std::string field;
    std::optional<double> runner_value;
    std::optional<double> controller_value;
};

template <statistics::PrimaryMetric Metric> struct VariantStatisticsEvidence {
    std::vector<core::ObservationId> observation_ids;
    statistics::DescriptiveStatistics<Metric> descriptive;
    statistics::OutlierFlaggingResult<Metric> outliers;
};

template <statistics::PrimaryMetric Metric> struct AuthoritativePairwiseStatistics {
    VariantStatisticsEvidence<Metric> first;
    VariantStatisticsEvidence<Metric> second;
    std::vector<kernel::DependentLoadLatencyBlockId> block_ids;
    std::vector<statistics::PairedLogEffect> ordered_effects;
    statistics::LogRatioComparison comparison;
    statistics::BlockBootstrapResult confidence_interval;
    statistics::ComparisonConclusion conclusion;
};

using PairwiseStatistics =
    std::variant<AuthoritativePairwiseStatistics<statistics::Bandwidth>,
                 AuthoritativePairwiseStatistics<statistics::DependentLoadLatency>>;

struct ControllerStoppingReplay {
    kernel::DependentLoadLatencyBlockId boundary_block_id;
    statistics::BlockBootstrapResult bootstrap;
    statistics::AdaptiveStoppingEvaluation evaluation;
    measurement::PrecisionStoppingDecision decision;
    bool exact_agreement;
};

class PairwiseRunResult {
  public:
    PairwiseRunResult(PairwiseRunResult&&) noexcept = default;
    PairwiseRunResult(const PairwiseRunResult&) = default;
    auto operator=(PairwiseRunResult&&) -> PairwiseRunResult& = delete;
    auto operator=(const PairwiseRunResult&) -> PairwiseRunResult& = delete;

    [[nodiscard]] auto provenance() const noexcept -> const runtime::PairwiseRunProvenance&;
    [[nodiscard]] auto statistics_policy() const noexcept
        -> const runtime::StatisticsProductionPolicy&;
    [[nodiscard]] auto monitoring_policy() const noexcept
        -> const monitoring::MonitoringExecutionPolicy&;
    [[nodiscard]] auto repetition_policy() const noexcept
        -> const measurement::RepetitionPolicy&;
    [[nodiscard]] auto comparison_order() const noexcept
        -> const measurement::ComparisonOrderPolicy&;
    [[nodiscard]] auto calibration() const noexcept
        -> const std::optional<runtime::PairwiseCalibrationEvidence>&;
    [[nodiscard]] auto warmup() const noexcept
        -> const std::optional<runtime::PairwiseWarmupEvidence>&;
    [[nodiscard]] auto terminal() const noexcept -> const runtime::PairwiseRunTerminalEvidence&;
    [[nodiscard]] auto validity() const noexcept -> PairwiseResultValidity;
    [[nodiscard]] auto authoritative_statistics() const noexcept
        -> const std::optional<PairwiseStatistics>&;
    [[nodiscard]] auto stopping_replays() const noexcept
        -> std::span<const ControllerStoppingReplay>;
    [[nodiscard]] auto final_calculation_failure() const noexcept
        -> const std::optional<FinalCalculationFailure>& {
        return final_failure_;
    }
    [[nodiscard]] auto diagnostics() const noexcept
        -> std::span<const RecalculationMismatchDiagnostic>;
    [[nodiscard]] auto raw_blocks() const noexcept
        -> std::span<const runtime::PairwiseBlockAttemptEvidence>;
    [[nodiscard]] auto finalization() const noexcept
        -> const std::optional<runtime::PairwiseFinalizationEvidence>&;

  private:
    PairwiseRunResult(runtime::PairwiseRunProvenance, runtime::StatisticsProductionPolicy,
                      monitoring::MonitoringExecutionPolicy, measurement::RepetitionPolicy,
                      measurement::ComparisonOrderPolicy,
                      std::optional<runtime::PairwiseCalibrationEvidence>,
                      std::optional<runtime::PairwiseWarmupEvidence>,
                      runtime::PairwiseRunTerminalEvidence,
                      std::optional<runtime::PairwiseFinalizationEvidence>,
                      std::vector<runtime::PairwiseBlockAttemptEvidence>, PairwiseResultValidity,
                      std::optional<PairwiseStatistics>, std::vector<ControllerStoppingReplay>,
                      std::vector<RecalculationMismatchDiagnostic>,
                      std::optional<FinalCalculationFailure> = std::nullopt) noexcept;
    runtime::PairwiseRunProvenance provenance_;
    runtime::StatisticsProductionPolicy statistics_policy_;
    monitoring::MonitoringExecutionPolicy monitoring_policy_;
    measurement::RepetitionPolicy repetition_policy_;
    measurement::ComparisonOrderPolicy comparison_order_;
    std::optional<runtime::PairwiseCalibrationEvidence> calibration_;
    std::optional<runtime::PairwiseWarmupEvidence> warmup_;
    runtime::PairwiseRunTerminalEvidence terminal_;
    std::optional<runtime::PairwiseFinalizationEvidence> finalization_;
    std::vector<runtime::PairwiseBlockAttemptEvidence> blocks_;
    PairwiseResultValidity validity_;
    std::optional<PairwiseStatistics> statistics_;
    std::vector<ControllerStoppingReplay> stopping_;
    std::vector<RecalculationMismatchDiagnostic> diagnostics_;
    std::optional<FinalCalculationFailure> final_failure_;
    friend auto project_pairwise_run_result(const runtime::FinalizedPairwiseRunEvidence&)
        -> std::expected<PairwiseRunResult, core::Error>;
    friend class detail::PreparedPairwiseRunResultAccess;
};

[[nodiscard]] auto project_pairwise_run_result(
    const runtime::FinalizedPairwiseRunEvidence& evidence)
    -> std::expected<PairwiseRunResult, core::Error>;

} // namespace epycbench::results
