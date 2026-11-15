#pragma once
#include "monitoring/host_q1_plan_v2_value.hpp"
#include "monitoring/host_q1_public_input_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1ValidatedPublicValuesTestAccess;
struct MonitoringQ1Q0FamilyAdmission final {
    std::string family;
    std::string schema;
    core::Sha256Digest sha256;
    std::string variant_role;
    core::Sha256Digest base_environment_sha256;
};
struct MonitoringQ1Q0PublicProjection final {
    std::string private_root;
    std::string repository_commit;
    core::Sha256Digest build_identity;
    std::string environment_schema;
    std::string environment_digest;
    core::Sha256Digest environment_artifact_sha256;
    std::string qualification_plan_schema;
    std::string qualification_plan_digest;
    std::string characterization_schema;
    core::Sha256Digest characterization_artifact_sha256;
    std::string production_inputs_schema;
    core::Sha256Digest production_inputs_sha256;
    std::string base_environment_schema;
    core::Sha256Digest base_environment_sha256;
    std::array<MonitoringQ1Q0FamilyAdmission, 2> family_admissions;
    std::array<std::uint64_t, 2> selected_work;
    core::Sha256Digest manifest_sha256;
    core::Sha256Digest checksums_sha256;
};
class MonitoringQ1ValidatedPublicValues final {
  public:
    MonitoringQ1ValidatedPublicValues(const MonitoringQ1ValidatedPublicValues&) = delete;
    MonitoringQ1ValidatedPublicValues(MonitoringQ1ValidatedPublicValues&&) noexcept = default;
    auto operator=(const MonitoringQ1ValidatedPublicValues&)
        -> MonitoringQ1ValidatedPublicValues& = delete;
    auto operator=(MonitoringQ1ValidatedPublicValues&&)
        -> MonitoringQ1ValidatedPublicValues& = delete;
    [[nodiscard]] static auto validate(const MonitoringQ1PublicInputAuthority&)
        -> std::expected<MonitoringQ1ValidatedPublicValues, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto qualification_id() const noexcept -> std::string_view {
        return plan_.qualification_id();
    }
    [[nodiscard]] auto plan() const noexcept -> const MonitoringQ1PlanV2Value& { return plan_; }
    [[nodiscard]] auto policy_identity() const noexcept -> std::string_view {
        return policy_identity_;
    }
    [[nodiscard]] auto policy_sha256() const noexcept -> std::string_view { return policy_sha_; }
    [[nodiscard]] auto profile_sha256() const noexcept -> std::string_view { return profile_sha_; }
    [[nodiscard]] auto q0() const noexcept -> const MonitoringQ1Q0PublicProjection& { return q0_; }

  private:
    static auto bounded_preflight(std::string_view) -> bool;
    static auto profile_schema_for_test(std::string_view, std::string_view) -> bool;
    MonitoringQ1ValidatedPublicValues(MonitoringQ1PlanV2Value plan, std::string identity,
                                      std::string policy_sha, std::string profile_sha,
                                      MonitoringQ1Q0PublicProjection q0) noexcept
        : plan_{std::move(plan)}, policy_identity_{std::move(identity)},
          policy_sha_{std::move(policy_sha)}, profile_sha_{std::move(profile_sha)},
          q0_{std::move(q0)} {}
    MonitoringQ1PlanV2Value plan_;
    std::string policy_identity_, policy_sha_, profile_sha_;
    MonitoringQ1Q0PublicProjection q0_;
    friend class MonitoringQ1ValidatedPublicValuesTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
