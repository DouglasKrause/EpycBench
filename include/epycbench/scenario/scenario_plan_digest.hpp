#pragma once

#include "epycbench/core/canonical_json.hpp"
#include "epycbench/scenario/matrix_expansion.hpp"

#include <compare>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace epycbench::scenario {

namespace detail {
struct ScenarioRetainedPlanTestAccess;
}

class ScenarioDigestedPlan;

inline constexpr std::string_view scenario_digest_profile =
    "epycbench.scenario-digest.v1";
inline constexpr std::string_view resolved_scenario_plan_digest_profile =
    "epycbench.resolved-scenario-plan-digest.v1";

class ScenarioDigest {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<ScenarioDigest, core::Error>;
    [[nodiscard]] auto text() const -> std::string;
    [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& {
        return digest_;
    }
    auto operator<=>(const ScenarioDigest&) const = default;

  private:
    explicit ScenarioDigest(core::Sha256Digest digest) : digest_{digest} {}
    core::Sha256Digest digest_;

    friend class ScenarioDigestedPlan;
    friend auto compute_scenario_plan_digests(MatrixExpandedScenario)
        -> std::expected<ScenarioDigestedPlan, core::Error>;
};

class ResolvedScenarioPlanDigest {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<ResolvedScenarioPlanDigest, core::Error>;
    [[nodiscard]] auto text() const -> std::string;
    [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte>;
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& {
        return digest_;
    }
    auto operator<=>(const ResolvedScenarioPlanDigest&) const = default;

  private:
    explicit ResolvedScenarioPlanDigest(core::Sha256Digest digest)
        : digest_{digest} {}
    core::Sha256Digest digest_;

    friend class ScenarioDigestedPlan;
    friend auto compute_scenario_plan_digests(MatrixExpandedScenario)
        -> std::expected<ScenarioDigestedPlan, core::Error>;
};

[[nodiscard]] auto scenario_semantic_json(const MatrixExpandedScenario& scenario)
    -> std::expected<nlohmann::ordered_json, core::Error>;

[[nodiscard]] auto resolved_scenario_plan_semantic_json(
    const MatrixExpandedScenario& scenario, const ScenarioDigest& scenario_digest)
    -> std::expected<nlohmann::ordered_json, core::Error>;

class ScenarioDigestedPlan {
  public:
    [[nodiscard]] auto matrix_expanded() const noexcept -> const MatrixExpandedScenario&;
    [[nodiscard]] auto scenario_digest() const noexcept -> const ScenarioDigest&;
    [[nodiscard]] auto resolved_scenario_plan_digest() const noexcept
        -> const ResolvedScenarioPlanDigest&;
    [[nodiscard]] auto canonical_scenario_json() const noexcept -> std::string_view;
    [[nodiscard]] auto canonical_resolved_scenario_plan_json() const noexcept
        -> std::string_view;

  private:
    ScenarioDigestedPlan(MatrixExpandedScenario scenario, ScenarioDigest scenario_digest,
                         ResolvedScenarioPlanDigest resolved_digest,
                         std::string canonical_scenario,
                         std::string canonical_resolved_plan);

    MatrixExpandedScenario scenario_;
    ScenarioDigest scenario_digest_;
    ResolvedScenarioPlanDigest resolved_digest_;
    std::string canonical_scenario_;
    std::string canonical_resolved_plan_;

    friend auto compute_scenario_plan_digests(MatrixExpandedScenario)
        -> std::expected<ScenarioDigestedPlan, core::Error>;
    friend struct detail::ScenarioRetainedPlanTestAccess;
};

[[nodiscard]] auto compute_scenario_plan_digests(MatrixExpandedScenario scenario)
    -> std::expected<ScenarioDigestedPlan, core::Error>;

} // namespace epycbench::scenario
