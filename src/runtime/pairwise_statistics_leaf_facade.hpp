#pragma once

#include "epycbench/core/quantity.hpp"
#include "epycbench/core/seed.hpp"
#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "epycbench/statistics/adaptive_stopping.hpp"
#include "epycbench/statistics/block_bootstrap.hpp"
#include "epycbench/statistics/block_representatives.hpp"
#include "epycbench/statistics/log_ratio_comparison.hpp"
#include "epycbench/statistics/primary_metric.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::runtime::detail {

enum class PairwiseStatisticsNumericOperation : std::uint8_t {
    primary_metric,
    midpoint,
    logarithm,
    effect,
    accumulation,
    compensation,
    compensated_sum,
    mean,
    back_transform,
    quantile_rank,
    quantile_result,
    bootstrap_log_endpoint,
    bootstrap_ratio_endpoint,
    stopping_interval_width,
    stopping_observed_relative_half_width,
};

using PairwiseStatisticsTransformOperation = double (*)(
    void*, PairwiseStatisticsNumericOperation, double) noexcept;
using PairwiseStatisticsIndexOperation = std::size_t (*)(void*, std::size_t) noexcept;
using PairwiseStatisticsPredicateOperation = bool (*)(void*, bool) noexcept;

struct PairwiseStatisticsAlgorithms final {
    void* context{};
    PairwiseStatisticsTransformOperation transform{};
    PairwiseStatisticsIndexOperation quantile_index{};
    PairwiseStatisticsPredicateOperation population_size_valid{};
};

class PairwiseStatisticsLeafFacade;

class PairwiseStatisticsWorkspace final {
  public:
    PairwiseStatisticsWorkspace(const PairwiseStatisticsWorkspace&) = delete;
    auto operator=(const PairwiseStatisticsWorkspace&) -> PairwiseStatisticsWorkspace& = delete;
    PairwiseStatisticsWorkspace(PairwiseStatisticsWorkspace&& other) noexcept;
    auto operator=(PairwiseStatisticsWorkspace&&) noexcept
        -> PairwiseStatisticsWorkspace&;
    ~PairwiseStatisticsWorkspace() = default;

    [[nodiscard]] static auto prepare(std::size_t maximum_effect_count,
                                      std::size_t maximum_replicate_count)
        -> PairwiseStatisticsWorkspace;

    [[nodiscard]] auto maximum_effect_count() const noexcept -> std::size_t {
        return resample_.size();
    }
    [[nodiscard]] auto maximum_replicate_count() const noexcept -> std::size_t {
        return replicate_estimates_.size();
    }

  private:
    PairwiseStatisticsWorkspace(std::vector<double> sorted, std::vector<double> resample,
                                std::vector<double> replicate_estimates) noexcept
        : sorted_{std::move(sorted)}, resample_{std::move(resample)},
          replicate_estimates_{std::move(replicate_estimates)} {}

    std::vector<double> sorted_;
    std::vector<double> resample_;
    std::vector<double> replicate_estimates_;

    friend class PairwiseStatisticsLeafFacade;
};

class PairwiseStatisticsLeafFacade final {
  public:
    constexpr PairwiseStatisticsLeafFacade() noexcept = default;
    explicit constexpr PairwiseStatisticsLeafFacade(
        PairwiseStatisticsAlgorithms algorithms) noexcept
        : algorithms_{algorithms} {}
    [[nodiscard]] auto bandwidth(core::ByteCount bytes, core::Duration elapsed) const noexcept
        -> std::expected<statistics::Bandwidth, PairwiseStatisticsFailure>;
    [[nodiscard]] auto dependent_load_latency(core::Duration elapsed,
                                              core::WorkCount completed_work) const noexcept
        -> std::expected<statistics::DependentLoadLatency, PairwiseStatisticsFailure>;
    [[nodiscard]] auto bandwidth_representatives(
        std::array<statistics::Bandwidth, 2> first,
        std::array<statistics::Bandwidth, 2> second) const noexcept
        -> std::expected<statistics::BlockRepresentatives<statistics::Bandwidth>,
                         PairwiseStatisticsFailure>;
    [[nodiscard]] auto dependent_load_latency_representatives(
        std::array<statistics::DependentLoadLatency, 2> first,
        std::array<statistics::DependentLoadLatency, 2> second) const noexcept
        -> std::expected<statistics::BlockRepresentatives<statistics::DependentLoadLatency>,
                         PairwiseStatisticsFailure>;
    [[nodiscard]] auto bandwidth_log_effect(
        const statistics::BlockRepresentatives<statistics::Bandwidth>& representatives)
        const noexcept -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure>;
    [[nodiscard]] auto dependent_load_latency_log_effect(
        const statistics::BlockRepresentatives<statistics::DependentLoadLatency>& representatives)
        const noexcept -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure>;
    [[nodiscard]] auto paired_log_effect(double value) const noexcept
        -> std::expected<statistics::PairedLogEffect, PairwiseStatisticsFailure>;
    [[nodiscard]] auto oriented_geometric_ratio(double value) const noexcept
        -> std::expected<statistics::OrientedGeometricRatio, PairwiseStatisticsFailure>;
    [[nodiscard]] auto aggregate(std::span<const statistics::PairedLogEffect> effects,
                                 PairwiseStatisticsWorkspace& workspace) const noexcept
        -> std::expected<statistics::LogRatioComparison, PairwiseStatisticsFailure>;
    [[nodiscard]] auto bootstrap_policy(std::uint64_t replicate_count, double confidence,
                                        core::DerivedSeed seed) const noexcept
        -> std::expected<statistics::BootstrapPolicy, PairwiseStatisticsFailure>;
    [[nodiscard]] auto type7_quantile(std::span<const double> sorted,
                                      double probability) const noexcept
        -> std::expected<double, PairwiseStatisticsFailure>;
    [[nodiscard]] auto log_interval(statistics::PairedLogEffect lower,
                                    statistics::PairedLogEffect upper) const noexcept
        -> std::expected<statistics::PairedLogEffectInterval, PairwiseStatisticsFailure>;
    [[nodiscard]] auto ratio_interval(statistics::OrientedGeometricRatio lower,
                                      statistics::OrientedGeometricRatio upper) const noexcept
        -> std::expected<statistics::OrientedGeometricRatioInterval,
                         PairwiseStatisticsFailure>;
    [[nodiscard]] auto bootstrap(std::span<const statistics::PairedLogEffect> effects,
                                 const statistics::BootstrapPolicy& policy,
                                 PairwiseStatisticsWorkspace& workspace) const noexcept
        -> std::expected<statistics::BlockBootstrapResult, PairwiseStatisticsFailure>;
    [[nodiscard]] auto relative_half_width_target(double value) const noexcept
        -> std::expected<statistics::RelativeHalfWidthTarget, PairwiseStatisticsFailure>;
    [[nodiscard]] auto stopping(
        statistics::OrientedGeometricRatio point_ratio,
        const statistics::OrientedGeometricRatioInterval& interval,
        statistics::RelativeHalfWidthTarget target) const noexcept
        -> std::expected<statistics::AdaptiveStoppingEvaluation, PairwiseStatisticsFailure>;

  private:
    PairwiseStatisticsAlgorithms algorithms_{};
};

} // namespace epycbench::runtime::detail
