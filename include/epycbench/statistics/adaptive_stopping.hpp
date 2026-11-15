#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/block_bootstrap.hpp"

#include <compare>
#include <expected>

namespace epycbench::statistics {

namespace detail {
struct AdaptiveStoppingAccess;
}

class RelativeHalfWidthTarget {
  public:
    [[nodiscard]] static auto create(double value) noexcept
        -> std::expected<RelativeHalfWidthTarget, core::Error>;

    [[nodiscard]] constexpr auto value() const noexcept -> double { return value_; }
    constexpr auto operator<=>(const RelativeHalfWidthTarget&) const noexcept = default;

  private:
    friend struct detail::AdaptiveStoppingAccess;
    explicit constexpr RelativeHalfWidthTarget(double value) noexcept : value_{value} {}
    double value_;
};

class AdaptiveStoppingEvaluation;

class ObservedRelativeHalfWidth {
  public:
    [[nodiscard]] constexpr auto value() const noexcept -> double { return value_; }
    constexpr auto operator<=>(const ObservedRelativeHalfWidth&) const noexcept = default;

  private:
    friend struct detail::AdaptiveStoppingAccess;
    explicit constexpr ObservedRelativeHalfWidth(double value) noexcept : value_{value} {}
    double value_;

    friend auto evaluate_adaptive_stopping(OrientedGeometricRatio point_ratio,
                                           const OrientedGeometricRatioInterval& interval,
                                           RelativeHalfWidthTarget target)
        -> std::expected<AdaptiveStoppingEvaluation, core::Error>;
};

enum class PrecisionTargetAttainment {
    target_met,
    target_not_met,
};

class AdaptiveStoppingEvaluation {
  public:
    constexpr AdaptiveStoppingEvaluation(ObservedRelativeHalfWidth observed_precision,
                                         PrecisionTargetAttainment attainment) noexcept
        : observed_precision_{observed_precision}, attainment_{attainment} {}

    [[nodiscard]] constexpr auto observed_precision() const noexcept
        -> ObservedRelativeHalfWidth {
        return observed_precision_;
    }
    [[nodiscard]] constexpr auto attainment() const noexcept -> PrecisionTargetAttainment {
        return attainment_;
    }
    constexpr auto operator<=>(const AdaptiveStoppingEvaluation&) const noexcept = default;

  private:
    ObservedRelativeHalfWidth observed_precision_;
    PrecisionTargetAttainment attainment_;
};

[[nodiscard]] auto evaluate_adaptive_stopping(
    OrientedGeometricRatio point_ratio, const OrientedGeometricRatioInterval& interval,
    RelativeHalfWidthTarget target) -> std::expected<AdaptiveStoppingEvaluation, core::Error>;

} // namespace epycbench::statistics
