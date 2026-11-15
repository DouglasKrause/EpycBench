#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/statistics/block_bootstrap.hpp"

#include <compare>
#include <expected>

namespace epycbench::statistics {

namespace detail {
struct PreparedEvidenceHeaderAccess;
}

class PracticalEffectThreshold {
  public:
    [[nodiscard]] static auto create(double fractional_effect) noexcept
        -> std::expected<PracticalEffectThreshold, core::Error>;

    [[nodiscard]] constexpr auto delta() const noexcept -> double { return delta_; }
    constexpr auto operator<=>(const PracticalEffectThreshold&) const noexcept = default;

  private:
    friend struct detail::PreparedEvidenceHeaderAccess;
    explicit constexpr PracticalEffectThreshold(double delta) noexcept : delta_{delta} {}
    double delta_;
};

enum class ComparisonConclusion {
    candidate_faster,
    baseline_faster,
    detectable_below_practical_threshold,
    inconclusive,
};

[[nodiscard]] auto classify_conclusion(PairedLogEffect point_estimate,
                                       const PairedLogEffectInterval& confidence_interval,
                                       PracticalEffectThreshold threshold) noexcept
    -> ComparisonConclusion;

} // namespace epycbench::statistics
