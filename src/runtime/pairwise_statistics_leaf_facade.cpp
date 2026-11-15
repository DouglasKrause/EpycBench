#include "runtime/pairwise_statistics_leaf_facade.hpp"

#include "statistics/adaptive_stopping_access.hpp"
#include "statistics/block_bootstrap_detail.hpp"
#include "statistics/bootstrap_access.hpp"
#include "statistics/log_ratio_access.hpp"
#include "statistics/midpoint.hpp"
#include "statistics/primary_metric_access.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto statistics_failure(PairwiseFailureCode code,
                                      PairwiseFailureOperation operation,
                                      PairwiseFailureReason reason) noexcept
    -> PairwiseStatisticsFailure {
    auto value = PairwiseStatisticsFailure::create(
        code, operation, reason, NoPairwiseFailureContext{}, std::nullopt);
    if (!value)
        std::terminate();
    return std::move(*value);
}

template <typename Value>
[[nodiscard]] auto failed(PairwiseFailureCode code, PairwiseFailureOperation operation,
                          PairwiseFailureReason reason) noexcept
    -> std::expected<Value, PairwiseStatisticsFailure> {
    return std::unexpected{statistics_failure(code, operation, reason)};
}

[[nodiscard]] auto transform(const PairwiseStatisticsAlgorithms& algorithms,
                             PairwiseStatisticsNumericOperation operation,
                             double value) noexcept -> double {
    return algorithms.transform == nullptr
               ? value
               : algorithms.transform(algorithms.context, operation, value);
}

template <typename Metric, typename MakeMetric>
[[nodiscard]] auto representatives(std::array<Metric, 2> first,
                                   std::array<Metric, 2> second, MakeMetric make_metric,
                                   const PairwiseStatisticsAlgorithms& algorithms) noexcept
    -> std::expected<statistics::BlockRepresentatives<Metric>, PairwiseStatisticsFailure> {
    const auto first_value = transform(algorithms,
        PairwiseStatisticsNumericOperation::midpoint,
        statistics::detail::difference_first_midpoint(
        statistics::detail::PrimaryMetricAccess::value(first[0]),
        statistics::detail::PrimaryMetricAccess::value(first[1])));
    const auto second_value = transform(algorithms,
        PairwiseStatisticsNumericOperation::midpoint,
        statistics::detail::difference_first_midpoint(
        statistics::detail::PrimaryMetricAccess::value(second[0]),
        statistics::detail::PrimaryMetricAccess::value(second[1])));
    if (!std::isfinite(first_value) || first_value <= 0.0 ||
        !std::isfinite(second_value) || second_value <= 0.0)
        return failed<statistics::BlockRepresentatives<Metric>>(
            PairwiseFailureCode::statistics_block_representative_numeric_failure,
            PairwiseFailureOperation::statistics_block_representative_calculate,
            PairwiseFailureReason::invalid_midpoint);
    return statistics::BlockRepresentatives<Metric>{make_metric(first_value),
                                                     make_metric(second_value)};
}

[[nodiscard]] auto calculate_effect(
    double minuend, double subtrahend,
    const PairwiseStatisticsAlgorithms& algorithms) noexcept
    -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure> {
    constexpr auto code = PairwiseFailureCode::statistics_log_ratio_numeric_failure;
    constexpr auto operation = PairwiseFailureOperation::statistics_log_ratio_compare;
    const auto minuend_log = transform(
        algorithms, PairwiseStatisticsNumericOperation::logarithm,
        std::log(minuend));
    const auto subtrahend_log = transform(
        algorithms, PairwiseStatisticsNumericOperation::logarithm,
        std::log(subtrahend));
    if (!std::isfinite(minuend_log) || !std::isfinite(subtrahend_log))
        return failed<statistics::PairedLogEffect>(
            code, operation, PairwiseFailureReason::nonfinite_logarithm);
    const auto effect = transform(
        algorithms, PairwiseStatisticsNumericOperation::effect,
        minuend_log - subtrahend_log);
    if (!std::isfinite(effect))
        return failed<statistics::PairedLogEffect>(
            code, operation, PairwiseFailureReason::nonfinite_effect);
    return statistics::detail::LogRatioAccess::paired(effect == 0.0 ? 0.0 : effect);
}

[[nodiscard]] auto aggregate_values(std::span<const double> values,
                                    std::span<double> sorted,
                                    const PairwiseStatisticsAlgorithms& algorithms) noexcept
    -> std::expected<statistics::LogRatioComparison, PairwiseStatisticsFailure> {
    constexpr auto code = PairwiseFailureCode::statistics_log_ratio_numeric_failure;
    constexpr auto operation = PairwiseFailureOperation::statistics_log_ratio_compare;
    std::ranges::copy(values, sorted.begin());
    std::ranges::sort(sorted);
    double sum{};
    double compensation{};
    for (const auto value : sorted) {
        const auto next = transform(
            algorithms, PairwiseStatisticsNumericOperation::accumulation,
            sum + value);
        if (!std::isfinite(next))
            return failed<statistics::LogRatioComparison>(
                code, operation, PairwiseFailureReason::nonfinite_accumulation);
        compensation += std::abs(sum) >= std::abs(value) ? (sum - next) + value
                                                          : (value - next) + sum;
        compensation = transform(
            algorithms, PairwiseStatisticsNumericOperation::compensation,
            compensation);
        if (!std::isfinite(compensation))
            return failed<statistics::LogRatioComparison>(
                code, operation, PairwiseFailureReason::nonfinite_compensation);
        sum = next;
    }
    const auto compensated = transform(
        algorithms, PairwiseStatisticsNumericOperation::compensated_sum,
        sum + compensation);
    if (!std::isfinite(compensated))
        return failed<statistics::LogRatioComparison>(
            code, operation, PairwiseFailureReason::nonfinite_compensated_sum);
    const auto mean = transform(
        algorithms, PairwiseStatisticsNumericOperation::mean,
        compensated / static_cast<double>(values.size()));
    if (!std::isfinite(mean))
        return failed<statistics::LogRatioComparison>(
            code, operation, PairwiseFailureReason::nonfinite_mean);
    const auto effect = statistics::detail::LogRatioAccess::paired(mean == 0.0 ? 0.0 : mean);
    const auto ratio_value = transform(
        algorithms, PairwiseStatisticsNumericOperation::back_transform,
        std::exp(effect.value()));
    if (!std::isfinite(ratio_value) || ratio_value <= 0.0)
        return failed<statistics::LogRatioComparison>(
            code, operation, PairwiseFailureReason::invalid_back_transform);
    return statistics::LogRatioComparison{
        effect, statistics::detail::LogRatioAccess::ratio(ratio_value)};
}

[[nodiscard]] auto quantile_reason(std::span<const double> sorted,
                                   double probability,
                                   const PairwiseStatisticsAlgorithms& algorithms) noexcept
    -> std::expected<double, PairwiseFailureReason> {
    if (sorted.empty() || !std::isfinite(probability) || probability < 0.0 || probability > 1.0)
        return std::unexpected{PairwiseFailureReason::invalid_quantile_input};
    const auto last_index = static_cast<double>(sorted.size() - 1U);
    const auto rank = transform(
        algorithms, PairwiseStatisticsNumericOperation::quantile_rank,
        last_index * probability);
    if (!std::isfinite(rank) || rank < 0.0 || rank > last_index)
        return std::unexpected{PairwiseFailureReason::invalid_quantile_rank};
    const auto natural_lower = static_cast<std::size_t>(std::floor(rank));
    const auto lower = algorithms.quantile_index == nullptr
                           ? natural_lower
                           : algorithms.quantile_index(algorithms.context,
                                                       natural_lower);
    if (lower >= sorted.size())
        return std::unexpected{PairwiseFailureReason::quantile_index_overflow};
    if (lower == sorted.size() - 1U)
        return sorted[lower];
    const auto fraction = rank - static_cast<double>(lower);
    const auto result = transform(
        algorithms, PairwiseStatisticsNumericOperation::quantile_result,
        sorted[lower] + fraction * (sorted[lower + 1U] - sorted[lower]));
    if (!std::isfinite(result))
        return std::unexpected{PairwiseFailureReason::nonfinite_quantile};
    return result == 0.0 ? 0.0 : result;
}

} // namespace

auto PairwiseStatisticsWorkspace::prepare(std::size_t maximum_effect_count,
                                          std::size_t maximum_replicate_count)
    -> PairwiseStatisticsWorkspace {
    return PairwiseStatisticsWorkspace{std::vector<double>(maximum_effect_count),
                                       std::vector<double>(maximum_effect_count),
                                       std::vector<double>(maximum_replicate_count)};
}

PairwiseStatisticsWorkspace::PairwiseStatisticsWorkspace(
    PairwiseStatisticsWorkspace&& other) noexcept
    : sorted_{std::move(other.sorted_)}, resample_{std::move(other.resample_)},
      replicate_estimates_{std::move(other.replicate_estimates_)} {
    other.sorted_.clear();
    other.resample_.clear();
    other.replicate_estimates_.clear();
}

auto PairwiseStatisticsWorkspace::operator=(PairwiseStatisticsWorkspace&& other) noexcept
    -> PairwiseStatisticsWorkspace& {
    if (this != &other) {
        sorted_ = std::move(other.sorted_);
        resample_ = std::move(other.resample_);
        replicate_estimates_ = std::move(other.replicate_estimates_);
        other.sorted_.clear();
        other.resample_.clear();
        other.replicate_estimates_.clear();
    }
    return *this;
}

auto PairwiseStatisticsLeafFacade::bandwidth(core::ByteCount bytes,
                                             core::Duration elapsed) const noexcept
    -> std::expected<statistics::Bandwidth, PairwiseStatisticsFailure> {
    constexpr auto operation = PairwiseFailureOperation::statistics_primary_metric_create;
    if (bytes.value() == 0U || elapsed.value() == 0U)
        return failed<statistics::Bandwidth>(
            PairwiseFailureCode::statistics_primary_metric_invalid_input, operation,
            PairwiseFailureReason::zero_bytes_or_duration);
    constexpr auto scale = 1'000'000'000.0;
    const auto value = transform(
        algorithms_, PairwiseStatisticsNumericOperation::primary_metric,
        static_cast<double>(bytes.value()) * scale /
            static_cast<double>(elapsed.value()));
    if (!std::isfinite(value) || value <= 0.0)
        return failed<statistics::Bandwidth>(
            PairwiseFailureCode::statistics_primary_metric_unrepresentable, operation,
            PairwiseFailureReason::nonfinite_or_nonpositive);
    return statistics::detail::PrimaryMetricAccess::bandwidth(value);
}

auto PairwiseStatisticsLeafFacade::dependent_load_latency(
    core::Duration elapsed, core::WorkCount completed_work) const noexcept
    -> std::expected<statistics::DependentLoadLatency, PairwiseStatisticsFailure> {
    constexpr auto operation = PairwiseFailureOperation::statistics_primary_metric_create;
    if (elapsed.value() == 0U || completed_work.value() == 0U)
        return failed<statistics::DependentLoadLatency>(
            PairwiseFailureCode::statistics_primary_metric_invalid_input, operation,
            PairwiseFailureReason::zero_duration_or_work);
    constexpr auto scale = 1'000'000'000.0;
    const auto value = transform(
        algorithms_, PairwiseStatisticsNumericOperation::primary_metric,
        static_cast<double>(elapsed.value()) /
            static_cast<double>(completed_work.value()) / scale);
    if (!std::isfinite(value) || value <= 0.0)
        return failed<statistics::DependentLoadLatency>(
            PairwiseFailureCode::statistics_primary_metric_unrepresentable, operation,
            PairwiseFailureReason::nonfinite_or_nonpositive);
    return statistics::detail::PrimaryMetricAccess::latency(value);
}

auto PairwiseStatisticsLeafFacade::bandwidth_representatives(
    std::array<statistics::Bandwidth, 2> first,
    std::array<statistics::Bandwidth, 2> second) const noexcept
    -> std::expected<statistics::BlockRepresentatives<statistics::Bandwidth>,
                     PairwiseStatisticsFailure> {
    return representatives(first, second, statistics::detail::PrimaryMetricAccess::bandwidth,
                           algorithms_);
}

auto PairwiseStatisticsLeafFacade::dependent_load_latency_representatives(
    std::array<statistics::DependentLoadLatency, 2> first,
    std::array<statistics::DependentLoadLatency, 2> second) const noexcept
    -> std::expected<statistics::BlockRepresentatives<statistics::DependentLoadLatency>,
                     PairwiseStatisticsFailure> {
    return representatives(first, second, statistics::detail::PrimaryMetricAccess::latency,
                           algorithms_);
}

auto PairwiseStatisticsLeafFacade::bandwidth_log_effect(
    const statistics::BlockRepresentatives<statistics::Bandwidth>& values) const noexcept
    -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure> {
    return calculate_effect(values.second().bytes_per_second(),
                            values.first().bytes_per_second(), algorithms_);
}

auto PairwiseStatisticsLeafFacade::dependent_load_latency_log_effect(
    const statistics::BlockRepresentatives<statistics::DependentLoadLatency>& values)
    const noexcept -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure> {
    return calculate_effect(values.first().seconds_per_completed_load(),
                            values.second().seconds_per_completed_load(), algorithms_);
}

auto PairwiseStatisticsLeafFacade::paired_log_effect(double value) const noexcept
    -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure> {
    if (!std::isfinite(value))
        return failed<statistics::PairedLogEffect>(
            PairwiseFailureCode::statistics_log_ratio_numeric_failure,
            PairwiseFailureOperation::statistics_log_ratio_compare,
            PairwiseFailureReason::invalid_paired_log_effect);
    return statistics::detail::LogRatioAccess::paired(value == 0.0 ? 0.0 : value);
}

auto PairwiseStatisticsLeafFacade::oriented_geometric_ratio(double value) const noexcept
    -> std::expected<statistics::OrientedGeometricRatio, PairwiseStatisticsFailure> {
    if (!std::isfinite(value) || value <= 0.0)
        return failed<statistics::OrientedGeometricRatio>(
            PairwiseFailureCode::statistics_log_ratio_numeric_failure,
            PairwiseFailureOperation::statistics_log_ratio_compare,
            PairwiseFailureReason::invalid_oriented_geometric_ratio);
    return statistics::detail::LogRatioAccess::ratio(value);
}

auto PairwiseStatisticsLeafFacade::aggregate(
    std::span<const statistics::PairedLogEffect> effects,
    PairwiseStatisticsWorkspace& workspace) const noexcept
    -> std::expected<statistics::LogRatioComparison, PairwiseStatisticsFailure> {
    if (effects.empty())
        return failed<statistics::LogRatioComparison>(
            PairwiseFailureCode::statistics_log_ratio_empty_input,
            PairwiseFailureOperation::statistics_log_ratio_compare,
            PairwiseFailureReason::empty_input);
    if (effects.size() > workspace.sorted_.size())
        std::terminate();
    const auto count = effects.size();
    for (std::size_t index{}; index < count; ++index)
        workspace.resample_[index] = effects[index].value();
    return aggregate_values(std::span<const double>{workspace.resample_.data(), count},
                            std::span<double>{workspace.sorted_.data(), count}, algorithms_);
}

auto PairwiseStatisticsLeafFacade::bootstrap_policy(std::uint64_t replicate_count,
                                                    double confidence,
                                                    core::DerivedSeed seed) const noexcept
    -> std::expected<statistics::BootstrapPolicy, PairwiseStatisticsFailure> {
    constexpr auto code = PairwiseFailureCode::statistics_bootstrap_invalid_configuration;
    constexpr auto operation = PairwiseFailureOperation::statistics_bootstrap_calculate;
    if (replicate_count == 0U || replicate_count > std::numeric_limits<std::size_t>::max())
        return failed<statistics::BootstrapPolicy>(
            code, operation, PairwiseFailureReason::invalid_replicate_count);
    if (!std::isfinite(confidence) || confidence <= 0.0 || confidence >= 1.0)
        return failed<statistics::BootstrapPolicy>(
            code, operation, PairwiseFailureReason::invalid_confidence);
    return statistics::detail::BootstrapAccess::policy(
        statistics::detail::BootstrapAccess::count(replicate_count),
        statistics::detail::BootstrapAccess::confidence(confidence), seed);
}

auto PairwiseStatisticsLeafFacade::type7_quantile(std::span<const double> sorted,
                                                  double probability) const noexcept
    -> std::expected<double, PairwiseStatisticsFailure> {
    const auto result = quantile_reason(sorted, probability, algorithms_);
    if (!result)
        return failed<double>(PairwiseFailureCode::statistics_bootstrap_numeric_failure,
                              PairwiseFailureOperation::statistics_bootstrap_calculate,
                              result.error());
    return *result;
}

auto PairwiseStatisticsLeafFacade::log_interval(statistics::PairedLogEffect lower,
                                                statistics::PairedLogEffect upper) const noexcept
    -> std::expected<statistics::PairedLogEffectInterval, PairwiseStatisticsFailure> {
    if (lower > upper)
        return failed<statistics::PairedLogEffectInterval>(
            PairwiseFailureCode::statistics_bootstrap_numeric_failure,
            PairwiseFailureOperation::statistics_bootstrap_calculate,
            PairwiseFailureReason::invalid_log_interval);
    return statistics::detail::BootstrapAccess::log_interval(lower, upper);
}

auto PairwiseStatisticsLeafFacade::ratio_interval(
    statistics::OrientedGeometricRatio lower,
    statistics::OrientedGeometricRatio upper) const noexcept
    -> std::expected<statistics::OrientedGeometricRatioInterval,
                     PairwiseStatisticsFailure> {
    if (lower > upper)
        return failed<statistics::OrientedGeometricRatioInterval>(
            PairwiseFailureCode::statistics_bootstrap_numeric_failure,
            PairwiseFailureOperation::statistics_bootstrap_calculate,
            PairwiseFailureReason::invalid_ratio_interval);
    return statistics::detail::BootstrapAccess::ratio_interval(lower, upper);
}

auto PairwiseStatisticsLeafFacade::bootstrap(
    std::span<const statistics::PairedLogEffect> effects,
    const statistics::BootstrapPolicy& policy,
    PairwiseStatisticsWorkspace& workspace) const noexcept
    -> std::expected<statistics::BlockBootstrapResult, PairwiseStatisticsFailure> {
    constexpr auto code = PairwiseFailureCode::statistics_bootstrap_numeric_failure;
    constexpr auto operation = PairwiseFailureOperation::statistics_bootstrap_calculate;
    if (effects.empty())
        return failed<statistics::BlockBootstrapResult>(
            PairwiseFailureCode::statistics_bootstrap_empty_input, operation,
            PairwiseFailureReason::empty_input);
    const auto population_valid =
        effects.size() <= std::numeric_limits<std::uint64_t>::max();
    if (!(algorithms_.population_size_valid == nullptr
              ? population_valid
              : algorithms_.population_size_valid(algorithms_.context,
                                                   population_valid)))
        return failed<statistics::BlockBootstrapResult>(
            code, operation, PairwiseFailureReason::population_size_overflow);
    const auto replicate_count = static_cast<std::size_t>(policy.replicate_count().value());
    if (effects.size() > workspace.resample_.size() ||
        effects.size() > workspace.sorted_.size() ||
        replicate_count > workspace.replicate_estimates_.size())
        std::terminate();
    statistics::detail::SplitMix64 generator{policy.seed().value()};
    for (std::size_t replicate{}; replicate < replicate_count; ++replicate) {
        for (std::size_t draw{}; draw < effects.size(); ++draw) {
            const auto index = statistics::detail::unbiased_index(
                static_cast<std::uint64_t>(effects.size()),
                [&generator] { return generator.next(); });
            workspace.resample_[draw] = effects[static_cast<std::size_t>(index)].value();
        }
        const auto estimate = aggregate_values(
            std::span<const double>{workspace.resample_.data(), effects.size()},
            std::span<double>{workspace.sorted_.data(), effects.size()}, algorithms_);
        if (!estimate)
            return failed<statistics::BlockBootstrapResult>(
                code, operation, PairwiseFailureReason::replicate_estimator_failure);
        workspace.replicate_estimates_[replicate] = estimate->mean_log_effect().value();
    }
    auto estimates = std::span<double>{workspace.replicate_estimates_.data(), replicate_count};
    std::ranges::sort(estimates);
    const auto alpha = 1.0 - policy.confidence().value();
    const auto lower_value = quantile_reason(estimates, alpha / 2.0, algorithms_);
    const auto upper_value = quantile_reason(estimates, 1.0 - alpha / 2.0, algorithms_);
    if (!lower_value || !upper_value)
        return failed<statistics::BlockBootstrapResult>(
            code, operation, PairwiseFailureReason::interval_quantile_failure);
    const auto checked_lower_value = transform(
        algorithms_, PairwiseStatisticsNumericOperation::bootstrap_log_endpoint,
        *lower_value);
    const auto checked_upper_value = transform(
        algorithms_, PairwiseStatisticsNumericOperation::bootstrap_log_endpoint,
        *upper_value);
    if (!std::isfinite(checked_lower_value) || !std::isfinite(checked_upper_value))
        return failed<statistics::BlockBootstrapResult>(
            code, operation, PairwiseFailureReason::invalid_log_interval_endpoint);
    const auto lower_effect = statistics::detail::LogRatioAccess::paired(checked_lower_value);
    const auto upper_effect = statistics::detail::LogRatioAccess::paired(checked_upper_value);
    if (lower_effect > upper_effect)
        return failed<statistics::BlockBootstrapResult>(
            code, operation, PairwiseFailureReason::invalid_log_interval);
    const auto lower_ratio_value = transform(
        algorithms_, PairwiseStatisticsNumericOperation::bootstrap_ratio_endpoint,
        std::exp(lower_effect.value()));
    const auto upper_ratio_value = transform(
        algorithms_, PairwiseStatisticsNumericOperation::bootstrap_ratio_endpoint,
        std::exp(upper_effect.value()));
    if (!std::isfinite(lower_ratio_value) || lower_ratio_value <= 0.0 ||
        !std::isfinite(upper_ratio_value) || upper_ratio_value <= 0.0)
        return failed<statistics::BlockBootstrapResult>(
            code, operation, PairwiseFailureReason::invalid_ratio_interval_endpoint);
    const auto lower_ratio = statistics::detail::LogRatioAccess::ratio(lower_ratio_value);
    const auto upper_ratio = statistics::detail::LogRatioAccess::ratio(upper_ratio_value);
    if (lower_ratio > upper_ratio)
        return failed<statistics::BlockBootstrapResult>(
            code, operation, PairwiseFailureReason::invalid_ratio_interval);
    return statistics::BlockBootstrapResult{
        policy, statistics::detail::BootstrapAccess::log_interval(lower_effect, upper_effect),
        statistics::detail::BootstrapAccess::ratio_interval(lower_ratio, upper_ratio)};
}

auto PairwiseStatisticsLeafFacade::relative_half_width_target(double value) const noexcept
    -> std::expected<statistics::RelativeHalfWidthTarget, PairwiseStatisticsFailure> {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        return failed<statistics::RelativeHalfWidthTarget>(
            PairwiseFailureCode::statistics_stopping_invalid_configuration,
            PairwiseFailureOperation::statistics_stopping_evaluate,
            PairwiseFailureReason::invalid_relative_half_width_target);
    return statistics::detail::AdaptiveStoppingAccess::target(value == 0.0 ? 0.0 : value);
}

auto PairwiseStatisticsLeafFacade::stopping(
    statistics::OrientedGeometricRatio point_ratio,
    const statistics::OrientedGeometricRatioInterval& interval,
    statistics::RelativeHalfWidthTarget target) const noexcept
    -> std::expected<statistics::AdaptiveStoppingEvaluation, PairwiseStatisticsFailure> {
    const auto point = point_ratio.value();
    const auto lower = interval.lower().value();
    const auto upper = interval.upper().value();
    const auto width = transform(
        algorithms_, PairwiseStatisticsNumericOperation::stopping_interval_width,
        upper - lower);
    constexpr auto code = PairwiseFailureCode::statistics_stopping_numeric_failure;
    constexpr auto operation = PairwiseFailureOperation::statistics_stopping_evaluate;
    if (!std::isfinite(width) || width < 0.0)
        return failed<statistics::AdaptiveStoppingEvaluation>(
            code, operation, PairwiseFailureReason::invalid_interval_width);
    double observed{};
    if (width == 0.0)
        observed = 0.0;
    else if (width <= point)
        observed = (width / point) / 2.0;
    else if (point <= std::numeric_limits<double>::max() / 2.0)
        observed = width / (point * 2.0);
    else
        observed = (width / point) / 2.0;
    observed = transform(
        algorithms_,
        PairwiseStatisticsNumericOperation::stopping_observed_relative_half_width,
        observed);
    if (!std::isfinite(observed) || observed < 0.0)
        return failed<statistics::AdaptiveStoppingEvaluation>(
            code, operation, PairwiseFailureReason::invalid_observed_relative_half_width);
    observed = observed == 0.0 ? 0.0 : observed;
    return statistics::AdaptiveStoppingEvaluation{
        statistics::detail::AdaptiveStoppingAccess::observed(observed),
        observed <= target.value() ? statistics::PrecisionTargetAttainment::target_met
                                   : statistics::PrecisionTargetAttainment::target_not_met};
}

} // namespace epycbench::runtime::detail
