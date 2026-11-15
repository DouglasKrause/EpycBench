#include "epycbench/statistics/prepared_statistics.hpp"

#include "statistics/block_bootstrap_detail.hpp"
#include "statistics/bootstrap_access.hpp"
#include "statistics/adaptive_stopping_access.hpp"
#include "statistics/log_ratio_access.hpp"
#include "statistics/midpoint.hpp"
#include "statistics/outlier_flagging_access.hpp"
#include "statistics/primary_metric_access.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace epycbench::statistics {
namespace {

using Failure = PreparedStatisticsFailure;

auto neumaier_sum(std::span<const double> values) noexcept
    -> std::expected<double, Failure> {
    double sum{};
    double compensation{};
    for (const double value : values) {
        const double next = sum + value;
        if (!std::isfinite(next)) return std::unexpected{Failure::numeric_failure};
        compensation += std::abs(sum) >= std::abs(value) ? (sum - next) + value
                                                         : (value - next) + sum;
        if (!std::isfinite(compensation))
            return std::unexpected{Failure::numeric_failure};
        sum = next;
    }
    const double result = sum + compensation;
    if (!std::isfinite(result)) return std::unexpected{Failure::numeric_failure};
    return result;
}

auto percentile(std::span<const double> sorted, std::uint64_t numerator) noexcept -> double {
    if (sorted.size() == 1U) return sorted.front();
    constexpr std::uint64_t denominator{100U};
    const auto last = static_cast<std::uint64_t>(sorted.size() - 1U);
    const auto quotient = last / denominator;
    const auto remainder = last % denominator;
    const auto index = quotient * numerator + (remainder * numerator) / denominator;
    const auto fractional = (remainder * numerator) % denominator;
    const auto position = static_cast<std::size_t>(index);
    if (position == sorted.size() - 1U || fractional == 0U) return sorted[position];
    const double fraction = static_cast<double>(fractional) /
                            static_cast<double>(denominator);
    return sorted[position] + fraction * (sorted[position + 1U] - sorted[position]);
}

template <PrimaryMetric Metric, typename MakeMetric>
auto summarize(std::span<const Metric> samples, PreparedDescriptiveWorkspace<Metric> workspace,
               MakeMetric make_metric) noexcept
    -> std::expected<DescriptiveStatistics<Metric>, Failure> {
    if (samples.empty()) return std::unexpected{Failure::invalid_input};
    if (workspace.sorted.size() < samples.size() || workspace.deviations.size() < samples.size())
        return std::unexpected{Failure::insufficient_workspace};
    auto sorted = workspace.sorted.first(samples.size());
    for (std::size_t index{}; index < samples.size(); ++index)
        sorted[index] = detail::PrimaryMetricAccess::value(samples[index]);
    std::ranges::sort(sorted);
    const auto sum = neumaier_sum(sorted);
    if (!sum) return std::unexpected{sum.error()};
    const auto count = static_cast<std::uint64_t>(samples.size());
    const double mean = *sum / static_cast<double>(count);
    if (!std::isfinite(mean) || mean <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    const auto center = sorted.size() / 2U;
    const double median = sorted.size() % 2U == 1U
        ? sorted[center]
        : detail::difference_first_midpoint(sorted[center - 1U], sorted[center]);
    auto deviations = workspace.deviations.first(samples.size());
    for (std::size_t index{}; index < sorted.size(); ++index) {
        const double delta = sorted[index] - mean;
        deviations[index] = delta * delta;
        if (!std::isfinite(deviations[index]))
            return std::unexpected{Failure::numeric_failure};
    }
    const auto squared_sum = neumaier_sum(deviations);
    if (!squared_sum) return std::unexpected{squared_sum.error()};
    const double deviation = std::sqrt(*squared_sum / static_cast<double>(count));
    const double coefficient = deviation / mean;
    const double p5 = percentile(sorted, 5U);
    const double p95 = percentile(sorted, 95U);
    if (!std::isfinite(median) || !std::isfinite(deviation) ||
        !std::isfinite(coefficient) || !std::isfinite(p5) || !std::isfinite(p95))
        return std::unexpected{Failure::numeric_failure};
    return DescriptiveStatistics<Metric>{count, make_metric(sorted.front()),
        make_metric(sorted.back()), make_metric(mean), make_metric(median),
        make_metric(deviation), coefficient, make_metric(p5), make_metric(p95)};
}

auto median(std::span<const double> sorted) noexcept -> double {
    const auto center = sorted.size() / 2U;
    return sorted.size() % 2U == 1U
        ? sorted[center]
        : detail::difference_first_midpoint(sorted[center - 1U], sorted[center]);
}

template <PrimaryMetric Metric, typename MakeMetric>
auto flag(std::span<const Metric> samples, PreparedOutlierWorkspace<Metric> workspace,
          MakeMetric make_metric) noexcept
    -> std::expected<OutlierFlaggingResult<Metric>, Failure> {
    if (samples.empty() || workspace.flags == nullptr)
        return std::unexpected{Failure::invalid_input};
    if (workspace.sorted.size() < samples.size() || workspace.deviations.size() < samples.size() ||
        workspace.flags->capacity() < samples.size())
        return std::unexpected{Failure::insufficient_workspace};
    auto sorted = workspace.sorted.first(samples.size());
    for (std::size_t index{}; index < samples.size(); ++index) {
        sorted[index] = detail::PrimaryMetricAccess::value(samples[index]);
        if (!std::isfinite(sorted[index]) || sorted[index] <= 0.0)
            return std::unexpected{Failure::numeric_failure};
    }
    std::ranges::sort(sorted);
    const double center = median(sorted);
    auto deviations = workspace.deviations.first(samples.size());
    for (std::size_t index{}; index < sorted.size(); ++index) {
        deviations[index] = std::abs(sorted[index] - center);
        if (!std::isfinite(deviations[index]))
            return std::unexpected{Failure::numeric_failure};
    }
    std::ranges::sort(deviations);
    const double mad = median(deviations);
    if (!std::isfinite(center) || center <= 0.0 || !std::isfinite(mad) || mad < 0.0)
        return std::unexpected{Failure::numeric_failure};
    workspace.flags->clear();
    for (const Metric metric : samples) {
        const double value = detail::PrimaryMetricAccess::value(metric);
        if (mad == 0.0) {
            workspace.flags->push_back(value < center ? OutlierFlagState::flagged_low
                : value > center ? OutlierFlagState::flagged_high
                                 : OutlierFlagState::not_flagged);
        } else {
            const double score = detail::modified_z_scaling * (value - center) / mad;
            if (!std::isfinite(score)) return std::unexpected{Failure::numeric_failure};
            workspace.flags->push_back(score < -detail::modified_z_threshold
                ? OutlierFlagState::flagged_low
                : score > detail::modified_z_threshold ? OutlierFlagState::flagged_high
                                                       : OutlierFlagState::not_flagged);
        }
    }
    return OutlierFlaggingResult<Metric>{OutlierFlaggingMethod::median_absolute_deviation,
        make_metric(center), make_metric(mad), std::move(*workspace.flags)};
}

auto quantile(std::span<const double> sorted, double probability) noexcept
    -> std::expected<double, Failure> {
    if (sorted.empty() || !std::isfinite(probability) || probability < 0.0 || probability > 1.0)
        return std::unexpected{Failure::numeric_failure};
    const double last = static_cast<double>(sorted.size() - 1U);
    const double rank = last * probability;
    if (!std::isfinite(rank) || rank < 0.0 || rank > last)
        return std::unexpected{Failure::numeric_failure};
    const auto lower = static_cast<std::size_t>(std::floor(rank));
    if (lower >= sorted.size()) return std::unexpected{Failure::numeric_failure};
    if (lower == sorted.size() - 1U) return sorted[lower];
    const double result = sorted[lower] + (rank - static_cast<double>(lower)) *
        (sorted[lower + 1U] - sorted[lower]);
    if (!std::isfinite(result)) return std::unexpected{Failure::numeric_failure};
    return result == 0.0 ? 0.0 : result;
}

} // namespace

auto make_bandwidth_prepared(core::ByteCount bytes, core::Duration elapsed) noexcept
    -> std::expected<Bandwidth, Failure> {
    if (bytes.value() == 0U || elapsed.value() == 0U)
        return std::unexpected{Failure::invalid_input};
    const double value = static_cast<double>(bytes.value()) * 1'000'000'000.0 /
                         static_cast<double>(elapsed.value());
    if (!std::isfinite(value) || value <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    return detail::PrimaryMetricAccess::bandwidth(value);
}

auto make_dependent_load_latency_prepared(core::Duration elapsed,
                                          core::WorkCount work) noexcept
    -> std::expected<DependentLoadLatency, Failure> {
    if (elapsed.value() == 0U || work.value() == 0U)
        return std::unexpected{Failure::invalid_input};
    const double value = static_cast<double>(elapsed.value()) /
                         static_cast<double>(work.value()) / 1'000'000'000.0;
    if (!std::isfinite(value) || value <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    return detail::PrimaryMetricAccess::latency(value);
}

template <PrimaryMetric Metric, typename Make>
auto representatives(std::array<Metric, 2> first, std::array<Metric, 2> second,
                     Make make) noexcept
    -> std::expected<BlockRepresentatives<Metric>, Failure> {
    const double first_value = detail::difference_first_midpoint(
        detail::PrimaryMetricAccess::value(first[0]),
        detail::PrimaryMetricAccess::value(first[1]));
    const double second_value = detail::difference_first_midpoint(
        detail::PrimaryMetricAccess::value(second[0]),
        detail::PrimaryMetricAccess::value(second[1]));
    if (!std::isfinite(first_value) || first_value <= 0.0 ||
        !std::isfinite(second_value) || second_value <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    return BlockRepresentatives<Metric>{make(first_value), make(second_value)};
}

auto block_bandwidth_representatives_prepared(std::array<Bandwidth, 2> first,
                                              std::array<Bandwidth, 2> second) noexcept
    -> std::expected<BlockRepresentatives<Bandwidth>, Failure> {
    return representatives(first, second, detail::PrimaryMetricAccess::bandwidth);
}

auto block_dependent_load_latency_representatives_prepared(
    std::array<DependentLoadLatency, 2> first,
    std::array<DependentLoadLatency, 2> second) noexcept
    -> std::expected<BlockRepresentatives<DependentLoadLatency>, Failure> {
    return representatives(first, second, detail::PrimaryMetricAccess::latency);
}

auto prepared_effect(double minuend, double subtrahend) noexcept
    -> std::expected<PairedLogEffect, Failure> {
    const double value = std::log(minuend) - std::log(subtrahend);
    if (!std::isfinite(value)) return std::unexpected{Failure::numeric_failure};
    return detail::LogRatioAccess::paired(value == 0.0 ? 0.0 : value);
}

auto bandwidth_log_effect_prepared(const BlockRepresentatives<Bandwidth>& value) noexcept
    -> std::expected<PairedLogEffect, Failure> {
    return prepared_effect(value.second().bytes_per_second(),
                           value.first().bytes_per_second());
}

auto dependent_load_latency_log_effect_prepared(
    const BlockRepresentatives<DependentLoadLatency>& value) noexcept
    -> std::expected<PairedLogEffect, Failure> {
    return prepared_effect(value.first().seconds_per_completed_load(),
                           value.second().seconds_per_completed_load());
}

auto evaluate_adaptive_stopping_prepared(
    OrientedGeometricRatio point_ratio, const OrientedGeometricRatioInterval& interval,
    RelativeHalfWidthTarget target) noexcept
    -> std::expected<AdaptiveStoppingEvaluation, Failure> {
    const double point = point_ratio.value();
    const double width = interval.upper().value() - interval.lower().value();
    if (!std::isfinite(width) || width < 0.0 || !std::isfinite(point) || point <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    double observed{};
    if (width != 0.0)
        observed = width <= point ? (width / point) / 2.0
            : point <= std::numeric_limits<double>::max() / 2.0
                ? width / (point * 2.0) : (width / point) / 2.0;
    if (!std::isfinite(observed) || observed < 0.0)
        return std::unexpected{Failure::numeric_failure};
    observed = observed == 0.0 ? 0.0 : observed;
    return AdaptiveStoppingEvaluation{detail::AdaptiveStoppingAccess::observed(observed),
        observed <= target.value() ? PrecisionTargetAttainment::target_met
                                   : PrecisionTargetAttainment::target_not_met};
}

auto summarize_bandwidth_prepared(std::span<const Bandwidth> samples,
                                  PreparedDescriptiveWorkspace<Bandwidth> workspace) noexcept
    -> std::expected<DescriptiveStatistics<Bandwidth>, Failure> {
    return summarize(samples, workspace, detail::PrimaryMetricAccess::bandwidth);
}

auto summarize_dependent_load_latency_prepared(
    std::span<const DependentLoadLatency> samples,
    PreparedDescriptiveWorkspace<DependentLoadLatency> workspace) noexcept
    -> std::expected<DescriptiveStatistics<DependentLoadLatency>, Failure> {
    return summarize(samples, workspace, detail::PrimaryMetricAccess::latency);
}

auto flag_bandwidth_outliers_prepared(std::span<const Bandwidth> samples,
                                      PreparedOutlierWorkspace<Bandwidth> workspace) noexcept
    -> std::expected<OutlierFlaggingResult<Bandwidth>, Failure> {
    return flag(samples, workspace, detail::PrimaryMetricAccess::bandwidth);
}

auto flag_dependent_load_latency_outliers_prepared(
    std::span<const DependentLoadLatency> samples,
    PreparedOutlierWorkspace<DependentLoadLatency> workspace) noexcept
    -> std::expected<OutlierFlaggingResult<DependentLoadLatency>, Failure> {
    return flag(samples, workspace, detail::PrimaryMetricAccess::latency);
}

auto aggregate_log_effects_prepared(std::span<const PairedLogEffect> effects,
                                    std::span<double> sorted) noexcept
    -> std::expected<LogRatioComparison, Failure> {
    if (effects.empty()) return std::unexpected{Failure::invalid_input};
    if (sorted.size() < effects.size())
        return std::unexpected{Failure::insufficient_workspace};
    sorted = sorted.first(effects.size());
    for (std::size_t index{}; index < effects.size(); ++index)
        sorted[index] = effects[index].value();
    std::ranges::sort(sorted);
    const auto sum = neumaier_sum(sorted);
    if (!sum) return std::unexpected{sum.error()};
    const double mean_value = *sum / static_cast<double>(effects.size());
    const double ratio_value = std::exp(mean_value);
    if (!std::isfinite(mean_value) || !std::isfinite(ratio_value) || ratio_value <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    return LogRatioComparison{detail::LogRatioAccess::paired(
        mean_value == 0.0 ? 0.0 : mean_value), detail::LogRatioAccess::ratio(ratio_value)};
}

auto bootstrap_log_effects_prepared(std::span<const PairedLogEffect> effects,
                                    const BootstrapPolicy& policy,
                                    PreparedBootstrapWorkspace workspace) noexcept
    -> std::expected<BlockBootstrapResult, Failure> {
    if (effects.empty()) return std::unexpected{Failure::invalid_input};
    const auto replicates = static_cast<std::size_t>(policy.replicate_count().value());
    if (workspace.resample.size() < effects.size() ||
        workspace.replicate_estimates.size() < replicates ||
        workspace.aggregate_scratch.size() < effects.size())
        return std::unexpected{Failure::insufficient_workspace};
    auto resample = workspace.resample.first(effects.size());
    auto estimates = workspace.replicate_estimates.first(replicates);
    detail::SplitMix64 generator{policy.seed().value()};
    for (std::size_t replicate{}; replicate < replicates; ++replicate) {
        for (std::size_t draw{}; draw < effects.size(); ++draw) {
            const auto index = detail::unbiased_index(static_cast<std::uint64_t>(effects.size()),
                [&generator] { return generator.next(); });
            resample[draw] = effects[static_cast<std::size_t>(index)];
        }
        const auto aggregate = aggregate_log_effects_prepared(resample,
            workspace.aggregate_scratch);
        if (!aggregate) return std::unexpected{aggregate.error()};
        estimates[replicate] = aggregate->mean_log_effect().value();
    }
    std::ranges::sort(estimates);
    const double alpha = 1.0 - policy.confidence().value();
    const auto lower_value = quantile(estimates, alpha / 2.0);
    const auto upper_value = quantile(estimates, 1.0 - alpha / 2.0);
    if (!lower_value || !upper_value) return std::unexpected{Failure::numeric_failure};
    if (*lower_value > *upper_value) return std::unexpected{Failure::numeric_failure};
    const double lower_ratio_value = std::exp(*lower_value);
    const double upper_ratio_value = std::exp(*upper_value);
    if (!std::isfinite(lower_ratio_value) || lower_ratio_value <= 0.0 ||
        !std::isfinite(upper_ratio_value) || upper_ratio_value <= 0.0)
        return std::unexpected{Failure::numeric_failure};
    const auto lower_effect = detail::LogRatioAccess::paired(*lower_value);
    const auto upper_effect = detail::LogRatioAccess::paired(*upper_value);
    return BlockBootstrapResult{policy,
        detail::BootstrapAccess::log_interval(lower_effect, upper_effect),
        detail::BootstrapAccess::ratio_interval(
            detail::LogRatioAccess::ratio(lower_ratio_value),
            detail::LogRatioAccess::ratio(upper_ratio_value))};
}

} // namespace epycbench::statistics
