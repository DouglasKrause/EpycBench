#include "epycbench/statistics/descriptive_statistics.hpp"
#include "statistics/midpoint.hpp"
#include "statistics/primary_metric_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Descriptive statistics calculation failed",
            .operation = "statistics.descriptive.summarize",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto neumaier_sum(std::span<const double> values) noexcept -> std::expected<double, core::Error> {
    double sum = 0.0;
    double compensation = 0.0;
    for (const double value : values) {
        const double next = sum + value;
        if (!std::isfinite(next)) {
            return std::unexpected{
                error("statistics.descriptive.numeric_failure", "nonfinite_accumulation")};
        }
        if (std::abs(sum) >= std::abs(value)) {
            compensation += (sum - next) + value;
        } else {
            compensation += (value - next) + sum;
        }
        if (!std::isfinite(compensation)) {
            return std::unexpected{
                error("statistics.descriptive.numeric_failure", "nonfinite_compensation")};
        }
        sum = next;
    }
    const double result = sum + compensation;
    if (!std::isfinite(result)) {
        return std::unexpected{
            error("statistics.descriptive.numeric_failure", "nonfinite_compensated_sum")};
    }
    return result;
}

auto percentile(std::span<const double> sorted, std::uint64_t numerator) noexcept -> double {
    if (sorted.size() == 1) {
        return sorted.front();
    }
    constexpr std::uint64_t denominator = 100;
    const auto last = static_cast<std::uint64_t>(sorted.size() - 1);
    const auto quotient = last / denominator;
    const auto remainder = last % denominator;
    const auto index = quotient * numerator + (remainder * numerator) / denominator;
    const auto fractional_rank = (remainder * numerator) % denominator;
    const auto i = static_cast<std::size_t>(index);
    if (i == sorted.size() - 1 || fractional_rank == 0) {
        return sorted[i];
    }
    const double fraction = static_cast<double>(fractional_rank) /
                            static_cast<double>(denominator);
    return sorted[i] + fraction * (sorted[i + 1] - sorted[i]);
}

template <PrimaryMetric Metric, typename MakeMetric>
auto summarize(std::span<const Metric> samples, MakeMetric make_metric)
    -> std::expected<DescriptiveStatistics<Metric>, core::Error> {
    if (samples.empty()) {
        return std::unexpected{
            error("statistics.descriptive.empty_input", "empty_input")};
    }
    if constexpr (std::numeric_limits<std::size_t>::max() >
                  std::numeric_limits<std::uint64_t>::max()) {
        if (samples.size() > std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected{
                error("statistics.descriptive.count_overflow", "count_overflow")};
        }
    }

    std::vector<double> sorted;
    sorted.reserve(samples.size());
    for (const Metric metric : samples) {
        sorted.push_back(detail::PrimaryMetricAccess::value(metric));
    }
    std::sort(sorted.begin(), sorted.end());
    const auto count = static_cast<std::uint64_t>(sorted.size());

    const auto sum = neumaier_sum(sorted);
    if (!sum) {
        return std::unexpected{sum.error()};
    }
    const double mean = *sum / static_cast<double>(count);
    if (!std::isfinite(mean) || mean <= 0.0) {
        return std::unexpected{
            error("statistics.descriptive.numeric_failure", "invalid_mean")};
    }

    const std::size_t center = sorted.size() / 2;
    const double median = sorted.size() % 2 == 1
                              ? sorted[center]
                              : detail::difference_first_midpoint(sorted[center - 1],
                                                                  sorted[center]);

    std::vector<double> squared_deviations;
    squared_deviations.reserve(sorted.size());
    for (const double value : sorted) {
        const double deviation = value - mean;
        const double squared = deviation * deviation;
        if (!std::isfinite(squared)) {
            return std::unexpected{
                error("statistics.descriptive.numeric_failure", "nonfinite_squared_deviation")};
        }
        squared_deviations.push_back(squared);
    }
    const auto squared_sum = neumaier_sum(squared_deviations);
    if (!squared_sum) {
        return std::unexpected{squared_sum.error()};
    }
    const double variance = *squared_sum / static_cast<double>(count);
    const double standard_deviation = std::sqrt(variance);
    const double coefficient_of_variation = standard_deviation / mean;
    const double p5 = percentile(sorted, 5);
    const double p95 = percentile(sorted, 95);
    if (!std::isfinite(median) || !std::isfinite(standard_deviation) ||
        !std::isfinite(coefficient_of_variation) || !std::isfinite(p5) ||
        !std::isfinite(p95)) {
        return std::unexpected{
            error("statistics.descriptive.numeric_failure", "nonfinite_result")};
    }

    return DescriptiveStatistics<Metric>{count,
                                         make_metric(sorted.front()),
                                         make_metric(sorted.back()),
                                         make_metric(mean),
                                         make_metric(median),
                                         make_metric(standard_deviation),
                                         coefficient_of_variation,
                                         make_metric(p5),
                                         make_metric(p95)};
}

} // namespace

auto summarize_bandwidth(std::span<const Bandwidth> samples)
    -> std::expected<DescriptiveStatistics<Bandwidth>, core::Error> {
    return summarize(samples, detail::PrimaryMetricAccess::bandwidth);
}

auto summarize_dependent_load_latency(std::span<const DependentLoadLatency> samples)
    -> std::expected<DescriptiveStatistics<DependentLoadLatency>, core::Error> {
    return summarize(samples, detail::PrimaryMetricAccess::latency);
}

auto summarize_task_throughput(std::span<const TaskThroughput> samples)
    -> std::expected<DescriptiveStatistics<TaskThroughput>, core::Error> {
    return summarize(samples, detail::PrimaryMetricAccess::task_throughput);
}

} // namespace epycbench::statistics
