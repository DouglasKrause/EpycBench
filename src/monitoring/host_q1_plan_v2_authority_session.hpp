#pragma once

#include "core/governed_repository_root_authority_access.hpp"
#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/monitoring/host_environment.hpp"
#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "epycbench/runtime/admitted_placement_request.hpp"
#include "monitoring/host_q1_git_command_owner.hpp"
#include "monitoring/host_q1_git_repository_metadata.hpp"
#include "monitoring/host_q1_plan_v2_value.hpp"
#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ1BoundedRetirement final {
    static constexpr std::size_t capacity = 63U;
    std::array<std::optional<core::Error>, capacity> failures{};
    std::size_t count{};
    bool overflowed{};
};
struct MonitoringQ1BoundedFailure final {
    core::Error primary;
    MonitoringQ1BoundedRetirement cleanup;
};
static_assert(std::is_nothrow_move_constructible_v<core::Error>);
static_assert(std::is_nothrow_move_assignable_v<core::Error>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<core::Error>>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<core::Error>>);
static_assert(std::is_nothrow_move_constructible_v<MonitoringQ1BoundedRetirement>);
static_assert(std::is_nothrow_move_assignable_v<MonitoringQ1BoundedRetirement>);
static_assert(std::is_nothrow_move_constructible_v<MonitoringQ1BoundedFailure>);
static_assert(std::is_nothrow_move_assignable_v<MonitoringQ1BoundedFailure>);

class MonitoringQ1ValidatedPublicValues;
class MonitoringQ1PlanV2AuthoritySessionTestAccess;
class MonitoringQ2AuthoritySession;
class MonitoringQ3AuthoritySession;
class MonitoringQ4AuthoritySession;

struct MonitoringQ1EvidenceProvenance final {
    std::string manifest_sha256;
    std::string checksums_sha256;
    std::string host_environment_sha256;
    std::string q0b_evidence_sha256;
    std::string host_environment_digest;
};

class MonitoringQ1CollectorPlan final {
  public:
    MonitoringQ1CollectorPlan(const MonitoringQ1CollectorPlan&) = delete;
    MonitoringQ1CollectorPlan(MonitoringQ1CollectorPlan&&) noexcept = default;
    auto operator=(const MonitoringQ1CollectorPlan&) -> MonitoringQ1CollectorPlan& = delete;
    auto operator=(MonitoringQ1CollectorPlan&&) -> MonitoringQ1CollectorPlan& = delete;

    [[nodiscard]] auto qualification_id() const noexcept -> std::string_view { return qid_; }
    [[nodiscard]] auto qualification_plan_digest() const noexcept -> const core::Sha256Digest& {
        return plan_digest_;
    }
    [[nodiscard]] auto repository_commit() const noexcept -> std::string_view { return commit_; }
    [[nodiscard]] auto profile_sha256() const noexcept -> std::string_view { return profile_sha_; }
    [[nodiscard]] auto plan_sha256() const noexcept -> std::string_view { return plan_sha_; }
    [[nodiscard]] auto worker_ids() const noexcept -> std::span<const core::WorkerId> {
        return workers_;
    }
    [[nodiscard]] auto observations_per_worker() const noexcept -> std::uint64_t { return n_; }
    [[nodiscard]] auto capacity() const noexcept -> std::uint64_t { return capacity_; }
    [[nodiscard]] auto total_observations() const noexcept -> std::uint64_t { return total_; }
    [[nodiscard]] auto execution_deadline_seconds() const noexcept -> std::uint64_t {
        return deadline_;
    }
    [[nodiscard]] auto maximum_successor_tree_bytes() const noexcept -> std::uint64_t {
        return tree_;
    }
    [[nodiscard]] auto evidence_limit_bytes() const noexcept -> std::uint64_t { return 8388608; }
    [[nodiscard]] auto envelope_limit_bytes() const noexcept -> std::uint64_t { return 10485760; }
    [[nodiscard]] auto build_manifest_limit_bytes() const noexcept -> std::uint64_t {
        return 1048576;
    }
    [[nodiscard]] auto derivation_mode() const noexcept -> MonitoringQ1PlanDerivationMode {
        return mode_;
    }
    [[nodiscard]] auto per_observation_allowance_ms() const noexcept -> std::uint64_t {
        return allowance_;
    }
    [[nodiscard]] auto fixed_reserve_seconds() const noexcept -> std::uint64_t { return reserve_; }
    [[nodiscard]] auto evidence_provenance() const noexcept
        -> const MonitoringQ1EvidenceProvenance& {
        return provenance_;
    }

  private:
    MonitoringQ1CollectorPlan(
        std::string, std::string, std::string, std::string, core::Sha256Digest,
        std::vector<core::WorkerId>, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
        std::uint64_t, MonitoringQ1PlanDerivationMode, std::uint64_t, std::uint64_t,
        std::optional<runtime::detail::MonitoringQ1AdmittedWorkerSnapshot> = std::nullopt,
        MonitoringQ1EvidenceProvenance = {}) noexcept;
    [[nodiscard]] auto take_worker_snapshot() && noexcept
        -> std::optional<runtime::detail::MonitoringQ1AdmittedWorkerSnapshot> {
        return std::move(worker_snapshot_);
    }
    std::string qid_, commit_, profile_sha_, plan_sha_;
    core::Sha256Digest plan_digest_;
    std::vector<core::WorkerId> workers_;
    std::uint64_t n_{}, capacity_{}, total_{}, deadline_{}, tree_{};
    MonitoringQ1PlanDerivationMode mode_{};
    std::uint64_t allowance_{}, reserve_{};
    std::optional<runtime::detail::MonitoringQ1AdmittedWorkerSnapshot> worker_snapshot_;
    MonitoringQ1EvidenceProvenance provenance_;
    friend class MonitoringQ1PlanV2AuthoritySession;
    friend class MonitoringQ1PlanV2AuthoritySessionTestAccess;
    friend class MonitoringQ2AuthoritySession;
    friend class MonitoringQ1PageFaultCollectorAccess;
};

class MonitoringQ1PlanV2AuthoritySession final {
  public:
    MonitoringQ1PlanV2AuthoritySession(const MonitoringQ1PlanV2AuthoritySession&) = delete;
    MonitoringQ1PlanV2AuthoritySession(MonitoringQ1PlanV2AuthoritySession&&) noexcept;
    auto operator=(const MonitoringQ1PlanV2AuthoritySession&)
        -> MonitoringQ1PlanV2AuthoritySession& = delete;
    auto operator=(MonitoringQ1PlanV2AuthoritySession&&)
        -> MonitoringQ1PlanV2AuthoritySession& = delete;
    ~MonitoringQ1PlanV2AuthoritySession() noexcept;

    [[nodiscard]] static auto authenticate(const std::filesystem::path&, std::string_view) noexcept
        -> std::expected<MonitoringQ1PlanV2AuthoritySession, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto authenticated_repository_commit() const noexcept -> std::string_view;
    [[nodiscard]] auto retire_before_binding() noexcept -> MonitoringQ1BoundedRetirement;
    [[nodiscard]] auto bind_fresh_plan(const RunnerContextAcquisitionSession&,
                                       const MonitoringHostQualificationPlan&,
                                       const runtime::AdmittedPlacementRequest&) noexcept
        -> std::expected<MonitoringQ1CollectorPlan, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto bind_fresh_plan_bounded(const RunnerContextAcquisitionSession&,
                                               const MonitoringHostQualificationPlan&,
                                               const runtime::AdmittedPlacementRequest&) noexcept
        -> std::expected<MonitoringQ1CollectorPlan, MonitoringQ1BoundedFailure>;

  private:
    class Impl;
    [[nodiscard]] static auto authenticate_shared(const std::filesystem::path&, std::string_view,
                                                  bool, const HostFrozenSourceLaunchBinding* = nullptr) noexcept
        -> std::expected<MonitoringQ1PlanV2AuthoritySession, MonitoringQ1GitCommandFailure>;
    explicit MonitoringQ1PlanV2AuthoritySession(std::unique_ptr<Impl>) noexcept;
    void ensure_active() const noexcept;
    [[nodiscard]] auto q3_recheck() noexcept -> std::expected<bool, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto q4_root() noexcept -> core::detail::OwnedGovernedRepositoryRootDescriptor&;
    [[nodiscard]] auto q4_executable() noexcept -> MonitoringQ1GitExecutable&;
    [[nodiscard]] auto q4_values() const noexcept -> const MonitoringQ1ValidatedPublicValues&;
    [[nodiscard]] auto take_completed_values(
        std::optional<MonitoringQ1ValidatedPublicValues>&) noexcept -> bool;
    [[nodiscard]] auto q4_metadata() noexcept -> MonitoringQ1GitRepositoryMetadata&;
    std::unique_ptr<Impl> implementation_;
    bool consumed_{};
    bool completed_values_taken_{};
    friend class MonitoringQ1PlanV2AuthoritySessionTestAccess;
    friend class MonitoringQ2AuthoritySession;
    friend class MonitoringQ3AuthoritySession;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
};

} // namespace epycbench::monitoring::qualification::detail
