#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"

#include <compare>
#include <expected>
#include <functional>
#include <string>
#include <variant>

namespace epycbench::runtime {

namespace detail { struct PlacementProvenanceConstructionAccess; }

class HostQualificationPolicyIdentity final {
  public:
    HostQualificationPolicyIdentity(const HostQualificationPolicyIdentity&) = default;
    auto operator=(const HostQualificationPolicyIdentity&) -> HostQualificationPolicyIdentity& = delete;
    [[nodiscard]] auto text() const noexcept -> const std::string& { return value_; }
    auto operator==(const HostQualificationPolicyIdentity&) const -> bool = default;
  private:
    explicit HostQualificationPolicyIdentity(std::string value) : value_{std::move(value)} {}
    std::string value_;
    friend struct detail::PlacementProvenanceConstructionAccess;
};

class HostEnvironmentDigest final {
  public:
    HostEnvironmentDigest(const HostEnvironmentDigest&) = default;
    auto operator=(const HostEnvironmentDigest&) -> HostEnvironmentDigest& = delete;
    [[nodiscard]] auto text() const -> std::string { return "sha256:" + digest_.to_hex(); }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    auto operator<=>(const HostEnvironmentDigest&) const = default;
  private:
    explicit HostEnvironmentDigest(core::Sha256Digest digest) : digest_{digest} {}
    core::Sha256Digest digest_;
    friend struct detail::PlacementProvenanceConstructionAccess;
};

class HostQualificationPlanDigest final {
  public:
    HostQualificationPlanDigest(const HostQualificationPlanDigest&) = default;
    auto operator=(const HostQualificationPlanDigest&) -> HostQualificationPlanDigest& = delete;
    [[nodiscard]] auto text() const -> std::string { return "sha256:" + digest_.to_hex(); }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    auto operator<=>(const HostQualificationPlanDigest&) const = default;
  private:
    explicit HostQualificationPlanDigest(core::Sha256Digest digest) : digest_{digest} {}
    core::Sha256Digest digest_;
    friend struct detail::PlacementProvenanceConstructionAccess;
};

class ScenarioPlacementProvenance final {
  public:
    ScenarioPlacementProvenance(const ScenarioPlacementProvenance&) = default;
    auto operator=(const ScenarioPlacementProvenance&) -> ScenarioPlacementProvenance& = delete;
    [[nodiscard]] auto scenario_digest() const noexcept -> const scenario::ScenarioDigest& { return scenario_; }
    [[nodiscard]] auto resolved_scenario_plan_digest() const noexcept -> const scenario::ResolvedScenarioPlanDigest& { return resolved_; }
    auto operator==(const ScenarioPlacementProvenance&) const -> bool = default;
  private:
    ScenarioPlacementProvenance(scenario::ScenarioDigest scenario,
                                scenario::ResolvedScenarioPlanDigest resolved)
        : scenario_{std::move(scenario)}, resolved_{std::move(resolved)} {}
    scenario::ScenarioDigest scenario_;
    scenario::ResolvedScenarioPlanDigest resolved_;
    friend struct detail::PlacementProvenanceConstructionAccess;
};

class HostQualificationPlanPlacementProvenance final {
  public:
    HostQualificationPlanPlacementProvenance(const HostQualificationPlanPlacementProvenance&) = default;
    auto operator=(const HostQualificationPlanPlacementProvenance&)
        -> HostQualificationPlanPlacementProvenance& = delete;
    [[nodiscard]] auto policy_identity() const noexcept -> const HostQualificationPolicyIdentity& { return policy_; }
    [[nodiscard]] auto template_sha256() const noexcept -> const core::Sha256Digest& { return template_; }
    [[nodiscard]] auto environment_digest() const noexcept -> const HostEnvironmentDigest& { return environment_; }
    [[nodiscard]] auto host_qualification_plan_digest() const noexcept -> const HostQualificationPlanDigest& { return plan_; }
    auto operator==(const HostQualificationPlanPlacementProvenance&) const -> bool = default;
  private:
    HostQualificationPlanPlacementProvenance(HostQualificationPolicyIdentity policy, core::Sha256Digest template_digest,
                                             HostEnvironmentDigest environment,
                                             HostQualificationPlanDigest plan)
        : policy_{std::move(policy)}, template_{template_digest}, environment_{environment}, plan_{plan} {}
    HostQualificationPolicyIdentity policy_;
    core::Sha256Digest template_;
    HostEnvironmentDigest environment_;
    HostQualificationPlanDigest plan_;
    friend struct detail::PlacementProvenanceConstructionAccess;
};

using PlacementProvenance = std::variant<ScenarioPlacementProvenance,
                                         HostQualificationPlanPlacementProvenance>;

[[nodiscard]] auto scenario_provenance(const PlacementProvenance&) noexcept
    -> std::expected<std::reference_wrapper<const ScenarioPlacementProvenance>, core::Error>;
[[nodiscard]] auto host_qualification_plan_provenance(const PlacementProvenance&) noexcept
    -> std::expected<std::reference_wrapper<const HostQualificationPlanPlacementProvenance>, core::Error>;

} // namespace epycbench::runtime
