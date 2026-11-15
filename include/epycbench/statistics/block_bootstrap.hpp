#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/seed.hpp"
#include "epycbench/statistics/log_ratio_comparison.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::statistics {

namespace detail {
struct BootstrapAccess;
}

class BootstrapReplicateCount {
  public:
    [[nodiscard]] static auto create(std::uint64_t value) noexcept
        -> std::expected<BootstrapReplicateCount, core::Error>;

    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    constexpr auto operator<=>(const BootstrapReplicateCount&) const noexcept = default;

  private:
    friend struct detail::BootstrapAccess;
    explicit constexpr BootstrapReplicateCount(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class BootstrapConfidence {
  public:
    [[nodiscard]] static auto create(double value) noexcept
        -> std::expected<BootstrapConfidence, core::Error>;

    [[nodiscard]] constexpr auto value() const noexcept -> double { return value_; }
    constexpr auto operator<=>(const BootstrapConfidence&) const noexcept = default;

  private:
    friend struct detail::BootstrapAccess;
    explicit constexpr BootstrapConfidence(double value) noexcept : value_{value} {}
    double value_;
};

class BootstrapPolicy {
  public:
    [[nodiscard]] static auto create(std::uint64_t replicate_count, double confidence,
                                     core::DerivedSeed seed) noexcept
        -> std::expected<BootstrapPolicy, core::Error>;

    [[nodiscard]] constexpr auto replicate_count() const noexcept -> BootstrapReplicateCount {
        return replicate_count_;
    }
    [[nodiscard]] constexpr auto confidence() const noexcept -> BootstrapConfidence {
        return confidence_;
    }
    [[nodiscard]] constexpr auto seed() const noexcept -> core::DerivedSeed { return seed_; }
    constexpr auto operator<=>(const BootstrapPolicy&) const noexcept = default;

  private:
    friend struct detail::BootstrapAccess;
    constexpr BootstrapPolicy(BootstrapReplicateCount replicate_count,
                              BootstrapConfidence confidence, core::DerivedSeed seed) noexcept
        : replicate_count_{replicate_count}, confidence_{confidence}, seed_{seed} {}

    BootstrapReplicateCount replicate_count_;
    BootstrapConfidence confidence_;
    core::DerivedSeed seed_;
};

class PairedLogEffectInterval {
  public:
    [[nodiscard]] static auto create(PairedLogEffect lower, PairedLogEffect upper) noexcept
        -> std::expected<PairedLogEffectInterval, core::Error>;

    [[nodiscard]] constexpr auto lower() const noexcept -> PairedLogEffect { return lower_; }
    [[nodiscard]] constexpr auto upper() const noexcept -> PairedLogEffect { return upper_; }
    constexpr auto operator<=>(const PairedLogEffectInterval&) const noexcept = default;

  private:
    friend struct detail::BootstrapAccess;
    constexpr PairedLogEffectInterval(PairedLogEffect lower, PairedLogEffect upper) noexcept
        : lower_{lower}, upper_{upper} {}

    PairedLogEffect lower_;
    PairedLogEffect upper_;
};

class OrientedGeometricRatioInterval {
  public:
    [[nodiscard]] static auto create(OrientedGeometricRatio lower,
                                     OrientedGeometricRatio upper) noexcept
        -> std::expected<OrientedGeometricRatioInterval, core::Error>;

    [[nodiscard]] constexpr auto lower() const noexcept -> OrientedGeometricRatio { return lower_; }
    [[nodiscard]] constexpr auto upper() const noexcept -> OrientedGeometricRatio { return upper_; }
    constexpr auto operator<=>(const OrientedGeometricRatioInterval&) const noexcept = default;

  private:
    friend struct detail::BootstrapAccess;
    constexpr OrientedGeometricRatioInterval(OrientedGeometricRatio lower,
                                             OrientedGeometricRatio upper) noexcept
        : lower_{lower}, upper_{upper} {}

    OrientedGeometricRatio lower_;
    OrientedGeometricRatio upper_;
};

class BlockBootstrapResult {
  public:
    constexpr BlockBootstrapResult(BootstrapPolicy policy, PairedLogEffectInterval log_interval,
                                   OrientedGeometricRatioInterval ratio_interval) noexcept
        : policy_{policy}, log_interval_{log_interval}, ratio_interval_{ratio_interval} {}

    [[nodiscard]] constexpr auto policy() const noexcept -> const BootstrapPolicy& {
        return policy_;
    }
    [[nodiscard]] constexpr auto log_effect_interval() const noexcept
        -> PairedLogEffectInterval {
        return log_interval_;
    }
    [[nodiscard]] constexpr auto oriented_geometric_ratio_interval() const noexcept
        -> OrientedGeometricRatioInterval {
        return ratio_interval_;
    }

  private:
    BootstrapPolicy policy_;
    PairedLogEffectInterval log_interval_;
    OrientedGeometricRatioInterval ratio_interval_;
};

[[nodiscard]] auto bootstrap_log_effects(std::span<const PairedLogEffect> effects,
                                         const BootstrapPolicy& policy)
    -> std::expected<BlockBootstrapResult, core::Error>;

} // namespace epycbench::statistics
