#include "epycbench/statistics/conclusion_classifier.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Conclusion classifier threshold construction failed",
            .operation = "statistics.conclusion.threshold",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto invalid_configuration(std::string classification) -> core::Error {
    return error("statistics.conclusion.invalid_configuration", std::move(classification));
}

auto numeric_failure(std::string classification) -> core::Error {
    return error("statistics.conclusion.numeric_failure", std::move(classification));
}

} // namespace

auto PracticalEffectThreshold::create(double fractional_effect) noexcept
    -> std::expected<PracticalEffectThreshold, core::Error> {
    if (!std::isfinite(fractional_effect) || fractional_effect < 0.0 ||
        fractional_effect > 1.0) {
        return std::unexpected{invalid_configuration("invalid_fractional_effect")};
    }

    const double delta = std::log1p(fractional_effect);
    if (!std::isfinite(delta) || delta < 0.0) {
        return std::unexpected{numeric_failure("invalid_log_threshold")};
    }
    return PracticalEffectThreshold{delta == 0.0 ? 0.0 : delta};
}

auto classify_conclusion(PairedLogEffect point_estimate,
                         const PairedLogEffectInterval& confidence_interval,
                         PracticalEffectThreshold threshold) noexcept -> ComparisonConclusion {
    const double point = point_estimate.value();
    const double lower = confidence_interval.lower().value();
    const double upper = confidence_interval.upper().value();
    const double delta = threshold.delta();

    if (lower > 0.0) {
        if (point > delta) {
            return ComparisonConclusion::candidate_faster;
        }
        if (point > 0.0) {
            return ComparisonConclusion::detectable_below_practical_threshold;
        }
        return ComparisonConclusion::inconclusive;
    }

    if (upper < 0.0) {
        if (point < -delta) {
            return ComparisonConclusion::baseline_faster;
        }
        if (point < 0.0) {
            return ComparisonConclusion::detectable_below_practical_threshold;
        }
    }

    return ComparisonConclusion::inconclusive;
}

} // namespace epycbench::statistics
