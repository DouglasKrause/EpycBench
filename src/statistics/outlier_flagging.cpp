#include "epycbench/statistics/outlier_flagging.hpp"
#include "statistics/outlier_flagging_access.hpp"
#include "statistics/primary_metric_access.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Outlier flagging failed",
            .operation = "statistics.outlier.flag",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto midpoint_median(std::span<const double> sorted) noexcept -> double {
    const auto center = sorted.size() / 2;
    return sorted.size() % 2 == 1
               ? sorted[center]
               : sorted[center - 1] + (sorted[center] - sorted[center - 1]) / 2.0;
}

template <PrimaryMetric Metric, typename MakeMetric>
auto flag_outliers(std::span<const Metric> samples, MakeMetric make_metric)
    -> std::expected<OutlierFlaggingResult<Metric>, core::Error> {
    if (samples.empty()) {
        return std::unexpected{error("statistics.outlier.empty_input", "empty_input")};
    }

    std::vector<double> sorted;
    sorted.reserve(samples.size());
    for (const Metric metric : samples) {
        const double value = detail::PrimaryMetricAccess::value(metric);
        if (!std::isfinite(value) || value <= 0.0) {
            return std::unexpected{
                error("statistics.outlier.numeric_failure", "invalid_metric_value")};
        }
        sorted.push_back(value);
    }
    std::sort(sorted.begin(), sorted.end());
    const double median = midpoint_median(sorted);
    if (!std::isfinite(median) || median <= 0.0) {
        return std::unexpected{error("statistics.outlier.numeric_failure", "invalid_median")};
    }

    std::vector<double> deviations;
    deviations.reserve(sorted.size());
    for (const double value : sorted) {
        const double deviation = std::abs(value - median);
        if (!std::isfinite(deviation)) {
            return std::unexpected{
                error("statistics.outlier.numeric_failure", "invalid_deviation")};
        }
        deviations.push_back(deviation);
    }
    std::sort(deviations.begin(), deviations.end());
    const double mad = midpoint_median(deviations);
    if (!std::isfinite(mad) || mad < 0.0) {
        return std::unexpected{error("statistics.outlier.numeric_failure", "invalid_mad")};
    }

    std::vector<OutlierFlagState> flags;
    flags.reserve(samples.size());
    for (const Metric metric : samples) {
        const double value = detail::PrimaryMetricAccess::value(metric);
        if (mad == 0.0) {
            flags.push_back(value < median   ? OutlierFlagState::flagged_low
                            : value > median ? OutlierFlagState::flagged_high
                                             : OutlierFlagState::not_flagged);
            continue;
        }
        const double score = detail::modified_z_scaling * (value - median) / mad;
        const auto state = detail::classify_modified_z_score(score);
        if (!state) {
            return std::unexpected{state.error()};
        }
        flags.push_back(*state);
    }

    return OutlierFlaggingResult<Metric>{OutlierFlaggingMethod::median_absolute_deviation,
                                         make_metric(median), make_metric(mad), std::move(flags)};
}

} // namespace

auto detail::classify_modified_z_score(double score) noexcept
    -> std::expected<OutlierFlagState, core::Error> {
    if (!std::isfinite(score)) {
        return std::unexpected{error("statistics.outlier.numeric_failure", "invalid_score")};
    }
    if (score < -modified_z_threshold) {
        return OutlierFlagState::flagged_low;
    }
    if (score > modified_z_threshold) {
        return OutlierFlagState::flagged_high;
    }
    return OutlierFlagState::not_flagged;
}

auto flag_bandwidth_outliers(std::span<const Bandwidth> samples)
    -> std::expected<OutlierFlaggingResult<Bandwidth>, core::Error> {
    return flag_outliers(samples, detail::PrimaryMetricAccess::bandwidth);
}

auto flag_dependent_load_latency_outliers(std::span<const DependentLoadLatency> samples)
    -> std::expected<OutlierFlaggingResult<DependentLoadLatency>, core::Error> {
    return flag_outliers(samples, detail::PrimaryMetricAccess::latency);
}

} // namespace epycbench::statistics
