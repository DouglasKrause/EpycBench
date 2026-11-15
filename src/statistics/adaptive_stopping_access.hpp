#pragma once

#include "epycbench/statistics/adaptive_stopping.hpp"

namespace epycbench::statistics::detail {

struct AdaptiveStoppingAccess final {
    [[nodiscard]] static constexpr auto target(double value) noexcept
        -> RelativeHalfWidthTarget {
        return RelativeHalfWidthTarget{value};
    }
    [[nodiscard]] static constexpr auto observed(double value) noexcept
        -> ObservedRelativeHalfWidth {
        return ObservedRelativeHalfWidth{value};
    }
};

} // namespace epycbench::statistics::detail
