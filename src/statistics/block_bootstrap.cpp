#include "epycbench/statistics/block_bootstrap.hpp"

#include "statistics/block_bootstrap_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace epycbench::statistics {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::statistics,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Block bootstrap calculation failed",
            .operation = "statistics.bootstrap.calculate",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto invalid_configuration(std::string classification) -> core::Error {
    return error("statistics.bootstrap.invalid_configuration", std::move(classification));
}

auto numeric_failure(std::string classification) -> core::Error {
    return error("statistics.bootstrap.numeric_failure", std::move(classification));
}

} // namespace

auto BootstrapReplicateCount::create(std::uint64_t value) noexcept
    -> std::expected<BootstrapReplicateCount, core::Error> {
    if (value == 0U || value > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{invalid_configuration("invalid_replicate_count")};
    }
    return BootstrapReplicateCount{value};
}

auto BootstrapConfidence::create(double value) noexcept
    -> std::expected<BootstrapConfidence, core::Error> {
    if (!std::isfinite(value) || value <= 0.0 || value >= 1.0) {
        return std::unexpected{invalid_configuration("invalid_confidence")};
    }
    return BootstrapConfidence{value};
}

auto BootstrapPolicy::create(std::uint64_t replicate_count, double confidence,
                             core::DerivedSeed seed) noexcept
    -> std::expected<BootstrapPolicy, core::Error> {
    const auto count = BootstrapReplicateCount::create(replicate_count);
    if (!count) {
        return std::unexpected{count.error()};
    }
    const auto confidence_value = BootstrapConfidence::create(confidence);
    if (!confidence_value) {
        return std::unexpected{confidence_value.error()};
    }
    return BootstrapPolicy{*count, *confidence_value, seed};
}

auto PairedLogEffectInterval::create(PairedLogEffect lower, PairedLogEffect upper) noexcept
    -> std::expected<PairedLogEffectInterval, core::Error> {
    if (lower > upper) {
        return std::unexpected{numeric_failure("invalid_log_interval")};
    }
    return PairedLogEffectInterval{lower, upper};
}

auto OrientedGeometricRatioInterval::create(OrientedGeometricRatio lower,
                                             OrientedGeometricRatio upper) noexcept
    -> std::expected<OrientedGeometricRatioInterval, core::Error> {
    if (lower > upper) {
        return std::unexpected{numeric_failure("invalid_ratio_interval")};
    }
    return OrientedGeometricRatioInterval{lower, upper};
}

auto detail::type7_quantile(std::span<const double> sorted, double probability)
    -> std::expected<double, core::Error> {
    if (sorted.empty() || !std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
        return std::unexpected{numeric_failure("invalid_quantile_input")};
    }
    const double last_index = static_cast<double>(sorted.size() - 1U);
    const double rank = last_index * probability;
    if (!std::isfinite(rank) || rank < 0.0 || rank > last_index) {
        return std::unexpected{numeric_failure("invalid_quantile_rank")};
    }
    const auto lower_index = static_cast<std::size_t>(std::floor(rank));
    if (lower_index >= sorted.size()) {
        return std::unexpected{numeric_failure("quantile_index_overflow")};
    }
    if (lower_index == sorted.size() - 1U) {
        return sorted[lower_index];
    }
    const double fraction = rank - static_cast<double>(lower_index);
    const double result = sorted[lower_index] +
                          fraction * (sorted[lower_index + 1U] - sorted[lower_index]);
    if (!std::isfinite(result)) {
        return std::unexpected{numeric_failure("nonfinite_quantile")};
    }
    return result == 0.0 ? 0.0 : result;
}

auto bootstrap_log_effects(std::span<const PairedLogEffect> effects,
                           const BootstrapPolicy& policy)
    -> std::expected<BlockBootstrapResult, core::Error> {
    if (effects.empty()) {
        return std::unexpected{error("statistics.bootstrap.empty_input", "empty_input")};
    }
    if (effects.size() > std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected{invalid_configuration("population_size_overflow")};
    }

    const auto population_size = static_cast<std::uint64_t>(effects.size());
    const auto replicate_count = static_cast<std::size_t>(policy.replicate_count().value());
    detail::SplitMix64 generator{policy.seed().value()};
    std::vector<PairedLogEffect> resample;
    resample.reserve(effects.size());
    std::vector<double> replicate_estimates;
    replicate_estimates.reserve(replicate_count);

    for (std::size_t replicate = 0; replicate < replicate_count; ++replicate) {
        resample.clear();
        for (std::size_t draw = 0; draw < effects.size(); ++draw) {
            const auto index = detail::unbiased_index(population_size, [&generator] {
                return generator.next();
            });
            resample.push_back(effects[static_cast<std::size_t>(index)]);
        }
        const auto aggregate = aggregate_log_effects(resample);
        if (!aggregate) {
            return std::unexpected{numeric_failure("replicate_estimator_failure")};
        }
        replicate_estimates.push_back(aggregate->mean_log_effect().value());
    }

    std::ranges::sort(replicate_estimates);
    const double alpha = 1.0 - policy.confidence().value();
    const auto lower_value = detail::type7_quantile(replicate_estimates, alpha / 2.0);
    const auto upper_value = detail::type7_quantile(replicate_estimates, 1.0 - alpha / 2.0);
    if (!lower_value || !upper_value) {
        return std::unexpected{numeric_failure("interval_quantile_failure")};
    }
    const auto lower_effect = PairedLogEffect::create(*lower_value);
    const auto upper_effect = PairedLogEffect::create(*upper_value);
    if (!lower_effect || !upper_effect) {
        return std::unexpected{numeric_failure("invalid_log_interval_endpoint")};
    }
    const auto log_interval = PairedLogEffectInterval::create(*lower_effect, *upper_effect);
    if (!log_interval) {
        return std::unexpected{log_interval.error()};
    }

    const auto lower_ratio = OrientedGeometricRatio::create(std::exp(lower_effect->value()));
    const auto upper_ratio = OrientedGeometricRatio::create(std::exp(upper_effect->value()));
    if (!lower_ratio || !upper_ratio) {
        return std::unexpected{numeric_failure("invalid_ratio_interval_endpoint")};
    }
    const auto ratio_interval =
        OrientedGeometricRatioInterval::create(*lower_ratio, *upper_ratio);
    if (!ratio_interval) {
        return std::unexpected{ratio_interval.error()};
    }
    return BlockBootstrapResult{policy, *log_interval, *ratio_interval};
}

} // namespace epycbench::statistics
