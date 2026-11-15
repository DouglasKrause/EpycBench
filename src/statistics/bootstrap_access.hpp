#pragma once

#include "epycbench/statistics/block_bootstrap.hpp"

namespace epycbench::statistics::detail {

struct BootstrapAccess final {
    [[nodiscard]] static constexpr auto count(std::uint64_t value) noexcept
        -> BootstrapReplicateCount {
        return BootstrapReplicateCount{value};
    }
    [[nodiscard]] static constexpr auto confidence(double value) noexcept
        -> BootstrapConfidence {
        return BootstrapConfidence{value};
    }
    [[nodiscard]] static constexpr auto policy(BootstrapReplicateCount count,
                                               BootstrapConfidence confidence,
                                               core::DerivedSeed seed) noexcept
        -> BootstrapPolicy {
        return BootstrapPolicy{count, confidence, seed};
    }
    [[nodiscard]] static constexpr auto log_interval(PairedLogEffect lower,
                                                     PairedLogEffect upper) noexcept
        -> PairedLogEffectInterval {
        return PairedLogEffectInterval{lower, upper};
    }
    [[nodiscard]] static constexpr auto ratio_interval(OrientedGeometricRatio lower,
                                                       OrientedGeometricRatio upper) noexcept
        -> OrientedGeometricRatioInterval {
        return OrientedGeometricRatioInterval{lower, upper};
    }
};

} // namespace epycbench::statistics::detail
