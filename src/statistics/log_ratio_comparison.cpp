#include "epycbench/statistics/log_ratio_comparison.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Log-ratio comparison calculation failed",
            .operation = "statistics.log_ratio.compare",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto numeric_error(std::string classification) -> core::Error {
    return error("statistics.log_ratio.numeric_failure", std::move(classification));
}

auto calculate_effect(double minuend, double subtrahend)
    -> std::expected<PairedLogEffect, core::Error> {
    const double minuend_log = std::log(minuend);
    const double subtrahend_log = std::log(subtrahend);
    if (!std::isfinite(minuend_log) || !std::isfinite(subtrahend_log)) {
        return std::unexpected{numeric_error("nonfinite_logarithm")};
    }
    const double effect = minuend_log - subtrahend_log;
    if (!std::isfinite(effect)) {
        return std::unexpected{numeric_error("nonfinite_effect")};
    }
    return PairedLogEffect::create(effect == 0.0 ? 0.0 : effect);
}

auto neumaier_sum(std::span<const double> values) -> std::expected<double, core::Error> {
    double sum = 0.0;
    double compensation = 0.0;
    for (const double value : values) {
        const double next = sum + value;
        if (!std::isfinite(next)) {
            return std::unexpected{numeric_error("nonfinite_accumulation")};
        }
        if (std::abs(sum) >= std::abs(value)) {
            compensation += (sum - next) + value;
        } else {
            compensation += (value - next) + sum;
        }
        if (!std::isfinite(compensation)) {
            return std::unexpected{numeric_error("nonfinite_compensation")};
        }
        sum = next;
    }
    const double result = sum + compensation;
    if (!std::isfinite(result)) {
        return std::unexpected{numeric_error("nonfinite_compensated_sum")};
    }
    return result;
}

} // namespace

auto PairedLogEffect::create(double value) noexcept
    -> std::expected<PairedLogEffect, core::Error> {
    if (!std::isfinite(value)) {
        return std::unexpected{numeric_error("invalid_paired_log_effect")};
    }
    return PairedLogEffect{value == 0.0 ? 0.0 : value};
}

auto OrientedGeometricRatio::create(double value) noexcept
    -> std::expected<OrientedGeometricRatio, core::Error> {
    if (!std::isfinite(value) || value <= 0.0) {
        return std::unexpected{numeric_error("invalid_oriented_geometric_ratio")};
    }
    return OrientedGeometricRatio{value};
}

auto bandwidth_log_effect(const BlockRepresentatives<Bandwidth>& representatives)
    -> std::expected<PairedLogEffect, core::Error> {
    return calculate_effect(representatives.second().bytes_per_second(),
                            representatives.first().bytes_per_second());
}

auto dependent_load_latency_log_effect(
    const BlockRepresentatives<DependentLoadLatency>& representatives)
    -> std::expected<PairedLogEffect, core::Error> {
    return calculate_effect(representatives.first().seconds_per_completed_load(),
                            representatives.second().seconds_per_completed_load());
}

auto aggregate_log_effects(std::span<const PairedLogEffect> effects)
    -> std::expected<LogRatioComparison, core::Error> {
    if (effects.empty()) {
        return std::unexpected{error("statistics.log_ratio.empty_input", "empty_input")};
    }

    std::vector<double> sorted;
    sorted.reserve(effects.size());
    for (const PairedLogEffect effect : effects) {
        sorted.push_back(effect.value());
    }
    std::sort(sorted.begin(), sorted.end());

    const auto sum = neumaier_sum(sorted);
    if (!sum) {
        return std::unexpected{sum.error()};
    }
    const double mean = *sum / static_cast<double>(sorted.size());
    if (!std::isfinite(mean)) {
        return std::unexpected{numeric_error("nonfinite_mean")};
    }
    const auto mean_effect = PairedLogEffect::create(mean);
    if (!mean_effect) {
        return std::unexpected{mean_effect.error()};
    }

    const double ratio_value = std::exp(mean_effect->value());
    const auto ratio = OrientedGeometricRatio::create(ratio_value);
    if (!ratio) {
        return std::unexpected{numeric_error("invalid_back_transform")};
    }
    return LogRatioComparison{*mean_effect, *ratio};
}

} // namespace epycbench::statistics
