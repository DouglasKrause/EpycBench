#pragma once

#include "epycbench/statistics/block_bootstrap.hpp"
#include "epycbench/statistics/descriptive_statistics.hpp"
#include "epycbench/statistics/outlier_flagging.hpp"
#include "epycbench/statistics/adaptive_stopping.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace epycbench::statistics {

enum class PreparedStatisticsFailure : std::uint8_t {
    insufficient_workspace,
    invalid_input,
    numeric_failure,
};

template <PrimaryMetric Metric> struct PreparedDescriptiveWorkspace final {
    std::span<double> sorted;
    std::span<double> deviations;
};

template <PrimaryMetric Metric> struct PreparedOutlierWorkspace final {
    std::span<double> sorted;
    std::span<double> deviations;
    std::vector<OutlierFlagState>* flags;
};

struct PreparedBootstrapWorkspace final {
    std::span<PairedLogEffect> resample;
    std::span<double> replicate_estimates;
    std::span<double> aggregate_scratch;
};

[[nodiscard]] auto make_bandwidth_prepared(core::ByteCount, core::Duration) noexcept
    -> std::expected<Bandwidth, PreparedStatisticsFailure>;
[[nodiscard]] auto make_dependent_load_latency_prepared(core::Duration, core::WorkCount) noexcept
    -> std::expected<DependentLoadLatency, PreparedStatisticsFailure>;
[[nodiscard]] auto block_bandwidth_representatives_prepared(
    std::array<Bandwidth, 2>, std::array<Bandwidth, 2>) noexcept
    -> std::expected<BlockRepresentatives<Bandwidth>, PreparedStatisticsFailure>;
[[nodiscard]] auto block_dependent_load_latency_representatives_prepared(
    std::array<DependentLoadLatency, 2>, std::array<DependentLoadLatency, 2>) noexcept
    -> std::expected<BlockRepresentatives<DependentLoadLatency>, PreparedStatisticsFailure>;
[[nodiscard]] auto bandwidth_log_effect_prepared(
    const BlockRepresentatives<Bandwidth>&) noexcept
    -> std::expected<PairedLogEffect, PreparedStatisticsFailure>;
[[nodiscard]] auto dependent_load_latency_log_effect_prepared(
    const BlockRepresentatives<DependentLoadLatency>&) noexcept
    -> std::expected<PairedLogEffect, PreparedStatisticsFailure>;
[[nodiscard]] auto evaluate_adaptive_stopping_prepared(
    OrientedGeometricRatio, const OrientedGeometricRatioInterval&,
    RelativeHalfWidthTarget) noexcept
    -> std::expected<AdaptiveStoppingEvaluation, PreparedStatisticsFailure>;

[[nodiscard]] auto summarize_bandwidth_prepared(
    std::span<const Bandwidth>, PreparedDescriptiveWorkspace<Bandwidth>) noexcept
    -> std::expected<DescriptiveStatistics<Bandwidth>, PreparedStatisticsFailure>;
[[nodiscard]] auto summarize_dependent_load_latency_prepared(
    std::span<const DependentLoadLatency>,
    PreparedDescriptiveWorkspace<DependentLoadLatency>) noexcept
    -> std::expected<DescriptiveStatistics<DependentLoadLatency>, PreparedStatisticsFailure>;

[[nodiscard]] auto flag_bandwidth_outliers_prepared(
    std::span<const Bandwidth>, PreparedOutlierWorkspace<Bandwidth>) noexcept
    -> std::expected<OutlierFlaggingResult<Bandwidth>, PreparedStatisticsFailure>;
[[nodiscard]] auto flag_dependent_load_latency_outliers_prepared(
    std::span<const DependentLoadLatency>,
    PreparedOutlierWorkspace<DependentLoadLatency>) noexcept
    -> std::expected<OutlierFlaggingResult<DependentLoadLatency>, PreparedStatisticsFailure>;

[[nodiscard]] auto aggregate_log_effects_prepared(
    std::span<const PairedLogEffect>, std::span<double> sorted) noexcept
    -> std::expected<LogRatioComparison, PreparedStatisticsFailure>;
[[nodiscard]] auto bootstrap_log_effects_prepared(
    std::span<const PairedLogEffect>, const BootstrapPolicy&,
    PreparedBootstrapWorkspace) noexcept
    -> std::expected<BlockBootstrapResult, PreparedStatisticsFailure>;

} // namespace epycbench::statistics
