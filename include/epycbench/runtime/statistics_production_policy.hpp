#pragma once

#include "epycbench/runtime/ipc/payloads.hpp"
#include "epycbench/statistics/adaptive_stopping.hpp"
#include "epycbench/statistics/conclusion_classifier.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace epycbench::runtime {

namespace detail {
struct PairwiseV2ContextualAdmissionAccess;
struct PairwiseV2AdmissionClassificationAccess;
struct FinalizedEvidenceWirePolicyAccess;
struct PreparedEvidenceHeaderAccess;
}

enum class StatisticsExperimentKind { comparison, matrix_comparison };

class StatisticsComparisonPolicySource {
  public:
    [[nodiscard]] static auto create(
        StatisticsExperimentKind experiment_kind, scenario::ExactRatio comparison_confidence,
        std::uint64_t bootstrap_samples, scenario::ExactRatio global_practical_effect,
        std::optional<scenario::ExactRatio> local_practical_effect) noexcept
        -> std::expected<StatisticsComparisonPolicySource, core::Error>;

    [[nodiscard]] constexpr auto experiment_kind() const noexcept -> StatisticsExperimentKind {
        return experiment_kind_;
    }
    [[nodiscard]] constexpr auto comparison_confidence() const noexcept
        -> const scenario::ExactRatio& {
        return comparison_confidence_;
    }
    [[nodiscard]] constexpr auto bootstrap_samples() const noexcept -> std::uint64_t {
        return bootstrap_samples_;
    }
    [[nodiscard]] constexpr auto global_practical_effect() const noexcept
        -> const scenario::ExactRatio& {
        return global_practical_effect_;
    }
    [[nodiscard]] constexpr auto local_practical_effect() const noexcept
        -> const std::optional<scenario::ExactRatio>& {
        return local_practical_effect_;
    }
    [[nodiscard]] constexpr auto effective_practical_effect() const noexcept
        -> const scenario::ExactRatio& {
        return local_practical_effect_ ? *local_practical_effect_ : global_practical_effect_;
    }

    auto operator<=>(const StatisticsComparisonPolicySource&) const = default;

  private:
    StatisticsComparisonPolicySource(StatisticsExperimentKind experiment_kind,
                                     scenario::ExactRatio comparison_confidence,
                                     std::uint64_t bootstrap_samples,
                                     scenario::ExactRatio global_practical_effect,
                                     std::optional<scenario::ExactRatio> local_practical_effect)
        : experiment_kind_{experiment_kind}, comparison_confidence_{comparison_confidence},
          bootstrap_samples_{bootstrap_samples},
          global_practical_effect_{global_practical_effect},
          local_practical_effect_{std::move(local_practical_effect)} {}

    StatisticsExperimentKind experiment_kind_;
    scenario::ExactRatio comparison_confidence_;
    std::uint64_t bootstrap_samples_;
    scenario::ExactRatio global_practical_effect_;
    std::optional<scenario::ExactRatio> local_practical_effect_;
};

class StatisticsProductionPolicy {
  public:
    [[nodiscard]] static auto create(
        const StatisticsComparisonPolicySource& source,
        const ipc::PlacementMeasurementInputs& measurement) noexcept
        -> std::expected<StatisticsProductionPolicy, core::Error>;

    [[nodiscard]] constexpr auto comparison_bootstrap_policy() const noexcept
        -> const statistics::BootstrapPolicy& {
        return comparison_bootstrap_policy_;
    }
    [[nodiscard]] constexpr auto stopping_bootstrap_policy() const noexcept
        -> const statistics::BootstrapPolicy& {
        return stopping_bootstrap_policy_;
    }
    [[nodiscard]] constexpr auto effective_practical_effect_fraction() const noexcept -> double {
        return effective_practical_effect_fraction_;
    }
    [[nodiscard]] constexpr auto practical_effect_threshold() const noexcept
        -> statistics::PracticalEffectThreshold {
        return practical_effect_threshold_;
    }
    [[nodiscard]] constexpr auto relative_half_width_target() const noexcept
        -> statistics::RelativeHalfWidthTarget {
        return relative_half_width_target_;
    }

    auto operator<=>(const StatisticsProductionPolicy&) const = default;

  private:
    StatisticsProductionPolicy(statistics::BootstrapPolicy comparison_bootstrap_policy,
                               statistics::BootstrapPolicy stopping_bootstrap_policy,
                               double effective_practical_effect_fraction,
                               statistics::PracticalEffectThreshold practical_effect_threshold,
                               statistics::RelativeHalfWidthTarget relative_half_width_target)
        : comparison_bootstrap_policy_{comparison_bootstrap_policy},
          stopping_bootstrap_policy_{stopping_bootstrap_policy},
          effective_practical_effect_fraction_{effective_practical_effect_fraction},
          practical_effect_threshold_{practical_effect_threshold},
          relative_half_width_target_{relative_half_width_target} {}

    statistics::BootstrapPolicy comparison_bootstrap_policy_;
    statistics::BootstrapPolicy stopping_bootstrap_policy_;
    double effective_practical_effect_fraction_;
    statistics::PracticalEffectThreshold practical_effect_threshold_;
    statistics::RelativeHalfWidthTarget relative_half_width_target_;

    friend struct detail::PairwiseV2ContextualAdmissionAccess;
    friend struct detail::PairwiseV2AdmissionClassificationAccess;
    friend struct detail::FinalizedEvidenceWirePolicyAccess;
    friend struct detail::PreparedEvidenceHeaderAccess;
};

} // namespace epycbench::runtime
