#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/block_representatives.hpp"

#include <compare>
#include <expected>
#include <span>

namespace epycbench::statistics {

namespace detail {
struct LogRatioAccess;
}

class PairedLogEffect {
  public:
    [[nodiscard]] static auto create(double value) noexcept
        -> std::expected<PairedLogEffect, core::Error>;

    [[nodiscard]] constexpr auto value() const noexcept -> double { return value_; }
    constexpr auto operator<=>(const PairedLogEffect&) const noexcept = default;

  private:
    friend struct detail::LogRatioAccess;
    constexpr explicit PairedLogEffect(double value) noexcept : value_{value} {}
    double value_;
};

class OrientedGeometricRatio {
  public:
    [[nodiscard]] static auto create(double value) noexcept
        -> std::expected<OrientedGeometricRatio, core::Error>;

    [[nodiscard]] constexpr auto value() const noexcept -> double { return value_; }
    constexpr auto operator<=>(const OrientedGeometricRatio&) const noexcept = default;

  private:
    friend struct detail::LogRatioAccess;
    constexpr explicit OrientedGeometricRatio(double value) noexcept : value_{value} {}
    double value_;
};

class LogRatioComparison {
  public:
    constexpr LogRatioComparison(PairedLogEffect mean_log_effect,
                                 OrientedGeometricRatio oriented_geometric_ratio) noexcept
        : mean_log_effect_{mean_log_effect},
          oriented_geometric_ratio_{oriented_geometric_ratio} {}

    [[nodiscard]] constexpr auto mean_log_effect() const noexcept -> PairedLogEffect {
        return mean_log_effect_;
    }
    [[nodiscard]] constexpr auto oriented_geometric_ratio() const noexcept
        -> OrientedGeometricRatio {
        return oriented_geometric_ratio_;
    }

  private:
    PairedLogEffect mean_log_effect_;
    OrientedGeometricRatio oriented_geometric_ratio_;
};

[[nodiscard]] auto bandwidth_log_effect(
    const BlockRepresentatives<Bandwidth>& representatives)
    -> std::expected<PairedLogEffect, core::Error>;

[[nodiscard]] auto dependent_load_latency_log_effect(
    const BlockRepresentatives<DependentLoadLatency>& representatives)
    -> std::expected<PairedLogEffect, core::Error>;

[[nodiscard]] auto task_throughput_log_effect(
    const BlockRepresentatives<TaskThroughput>& representatives)
    -> std::expected<PairedLogEffect, core::Error>;

[[nodiscard]] auto aggregate_log_effects(std::span<const PairedLogEffect> effects)
    -> std::expected<LogRatioComparison, core::Error>;

} // namespace epycbench::statistics
