#pragma once
#include "epycbench/results/pairwise_run_result.hpp"

namespace epycbench::results::detail {
// Immutable identity mapping shared with serialization; no test fault definitions.
constexpr auto calculation_identity(CalculationStage stage) noexcept -> StatisticsFailureIdentity {
    using enum CalculationStage;
    switch (stage) {
    case primary_metric:
        return StatisticsFailureIdentity::primary_metric_unrepresentable;
    case block_representatives:
        return StatisticsFailureIdentity::block_representative_numeric_failure;
    case first_summary:
    case second_summary:
        return StatisticsFailureIdentity::descriptive_numeric_failure;
    case first_outliers:
    case second_outliers:
        return StatisticsFailureIdentity::outlier_numeric_failure;
    case paired_effect:
    case comparison:
    case stopping_comparison:
        return StatisticsFailureIdentity::log_ratio_numeric_failure;
    case comparison_bootstrap:
    case stopping_bootstrap:
        return StatisticsFailureIdentity::bootstrap_numeric_failure;
    case stopping_evaluation:
        return StatisticsFailureIdentity::stopping_numeric_failure;
    }
    return StatisticsFailureIdentity::log_ratio_numeric_failure;
}
} // namespace epycbench::results::detail
