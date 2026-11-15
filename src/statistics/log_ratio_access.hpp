#pragma once

#include "epycbench/statistics/log_ratio_comparison.hpp"

namespace epycbench::statistics::detail {

struct LogRatioAccess final {
    [[nodiscard]] static constexpr auto paired(double value) noexcept -> PairedLogEffect {
        return PairedLogEffect{value};
    }
    [[nodiscard]] static constexpr auto ratio(double value) noexcept
        -> OrientedGeometricRatio {
        return OrientedGeometricRatio{value};
    }
};

} // namespace epycbench::statistics::detail
