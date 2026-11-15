#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/descriptive_statistics.hpp"

#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::statistics {

enum class OutlierFlaggingMethod { median_absolute_deviation };

enum class OutlierFlagState { not_flagged, flagged_low, flagged_high };

template <PrimaryMetric Metric> class OutlierFlaggingResult {
  public:
    OutlierFlaggingResult(OutlierFlaggingMethod method, Metric median,
                          Metric median_absolute_deviation,
                          std::vector<OutlierFlagState> flags) noexcept
        : method_{method}, median_{median}, median_absolute_deviation_{median_absolute_deviation},
          flags_{std::move(flags)} {}

    [[nodiscard]] constexpr auto method() const noexcept -> OutlierFlaggingMethod {
        return method_;
    }
    [[nodiscard]] constexpr auto median() const noexcept -> Metric { return median_; }
    [[nodiscard]] constexpr auto median_absolute_deviation() const noexcept -> Metric {
        return median_absolute_deviation_;
    }
    [[nodiscard]] auto flags() const noexcept -> const std::vector<OutlierFlagState>& {
        return flags_;
    }

  private:
    OutlierFlaggingMethod method_;
    Metric median_;
    Metric median_absolute_deviation_;
    std::vector<OutlierFlagState> flags_;
};

[[nodiscard]] auto flag_bandwidth_outliers(std::span<const Bandwidth> samples)
    -> std::expected<OutlierFlaggingResult<Bandwidth>, core::Error>;

[[nodiscard]] auto flag_dependent_load_latency_outliers(
    std::span<const DependentLoadLatency> samples)
    -> std::expected<OutlierFlaggingResult<DependentLoadLatency>, core::Error>;

} // namespace epycbench::statistics
