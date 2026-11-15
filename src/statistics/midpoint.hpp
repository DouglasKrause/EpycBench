#pragma once

#include <algorithm>

namespace epycbench::statistics::detail {

[[nodiscard]] constexpr auto difference_first_midpoint(double first, double second) noexcept
    -> double {
    const double lo = std::min(first, second);
    const double hi = std::max(first, second);
    return lo + (hi - lo) / 2.0;
}

} // namespace epycbench::statistics::detail
