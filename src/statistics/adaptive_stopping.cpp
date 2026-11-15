#include "epycbench/statistics/adaptive_stopping.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Adaptive stopping precision evaluation failed",
            .operation = "statistics.stopping.evaluate",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto invalid_configuration(std::string classification) -> core::Error {
    return error("statistics.stopping.invalid_configuration", std::move(classification));
}

auto numeric_failure(std::string classification) -> core::Error {
    return error("statistics.stopping.numeric_failure", std::move(classification));
}

} // namespace

auto RelativeHalfWidthTarget::create(double value) noexcept
    -> std::expected<RelativeHalfWidthTarget, core::Error> {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        return std::unexpected{invalid_configuration("invalid_relative_half_width_target")};
    }
    return RelativeHalfWidthTarget{value == 0.0 ? 0.0 : value};
}

auto evaluate_adaptive_stopping(OrientedGeometricRatio point_ratio,
                                const OrientedGeometricRatioInterval& interval,
                                RelativeHalfWidthTarget target)
    -> std::expected<AdaptiveStoppingEvaluation, core::Error> {
    const double point = point_ratio.value();
    const double lower = interval.lower().value();
    const double upper = interval.upper().value();
    const double width = upper - lower;
    if (!std::isfinite(width) || width < 0.0) {
        return std::unexpected{numeric_failure("invalid_interval_width")};
    }

    double observed = 0.0;
    if (width == 0.0) {
        observed = 0.0;
    } else if (width <= point) {
        observed = (width / point) / 2.0;
    } else if (point <= std::numeric_limits<double>::max() / 2.0) {
        observed = width / (point * 2.0);
    } else {
        observed = (width / point) / 2.0;
    }

    if (!std::isfinite(observed) || observed < 0.0) {
        return std::unexpected{numeric_failure("invalid_observed_relative_half_width")};
    }
    observed = observed == 0.0 ? 0.0 : observed;
    const auto attainment = observed <= target.value()
                                 ? PrecisionTargetAttainment::target_met
                                 : PrecisionTargetAttainment::target_not_met;
    return AdaptiveStoppingEvaluation{ObservedRelativeHalfWidth{observed}, attainment};
}

} // namespace epycbench::statistics
