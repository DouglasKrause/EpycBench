#pragma once

#include "monitoring/host_q1_plan_v2_authority_session.hpp"

#include <memory>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3PlanPublicInputAuthority;
class MonitoringQ3CompleteQ2PredecessorAuthority;
class MonitoringRunningBuildAuthority;
class MonitoringQ3CompositionOwnerTestAccess;
class MonitoringQ3AuthoritySession;

struct MonitoringQ3CompositionFailure final {
    core::Error primary;
    MonitoringQ1BoundedRetirement cleanup;
};

class MonitoringQ3CompositionOwner final {
  public:
    static auto create() noexcept
        -> std::expected<MonitoringQ3CompositionOwner, MonitoringQ1GitCommandFailure>;
    MonitoringQ3CompositionOwner(MonitoringQ3CompositionOwner&&) noexcept;
    MonitoringQ3CompositionOwner(const MonitoringQ3CompositionOwner&) = delete;
    ~MonitoringQ3CompositionOwner() noexcept;
    void adopt_repository(MonitoringQ1PlanV2AuthoritySession) noexcept;
    void adopt_input(MonitoringQ3PlanPublicInputAuthority) noexcept;
    void adopt_predecessor(MonitoringQ3CompleteQ2PredecessorAuthority) noexcept;
    void adopt_build(MonitoringRunningBuildAuthority) noexcept;
    auto repository() noexcept -> MonitoringQ1PlanV2AuthoritySession&;
    auto take_repository() noexcept -> MonitoringQ1PlanV2AuthoritySession;
    auto input() noexcept -> MonitoringQ3PlanPublicInputAuthority&;
    auto take_input() noexcept -> MonitoringQ3PlanPublicInputAuthority;
    auto predecessor() noexcept -> MonitoringQ3CompleteQ2PredecessorAuthority&;
    auto take_predecessor() noexcept -> MonitoringQ3CompleteQ2PredecessorAuthority;
    auto build() noexcept -> MonitoringRunningBuildAuthority&;
    auto take_build() noexcept -> MonitoringRunningBuildAuthority;
    auto rollback(core::Error) noexcept -> MonitoringQ3CompositionFailure;
    auto retire() noexcept -> MonitoringQ1BoundedRetirement;
    auto take_cleanup() noexcept -> MonitoringQ1BoundedRetirement;

  private:
    class Impl;
    explicit MonitoringQ3CompositionOwner(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    void adopt_test(std::size_t, void*, std::optional<core::Error> (*)(void*) noexcept) noexcept;
    auto rollback_git(MonitoringQ1GitCommandFailure, bool) noexcept
        -> MonitoringQ3CompositionFailure;
    void seed_cleanup(core::Error) noexcept;
    void note_cleanup_overflow() noexcept;
    friend class MonitoringQ3CompositionOwnerTestAccess;
    friend class MonitoringQ3AuthoritySession;
};
class MonitoringQ3CompositionOwnerTestAccess final {
  public:
    static void adopt(MonitoringQ3CompositionOwner& owner, std::size_t slot, void* context,
        std::optional<core::Error> (*retire)(void*) noexcept) {
        owner.adopt_test(slot, context, retire);
    }
    static auto rollback(MonitoringQ3CompositionOwner& owner,
                         MonitoringQ1GitCommandFailure failure, bool overflowed = false) noexcept {
        return owner.rollback_git(std::move(failure), overflowed);
    }
    static void seed_cleanup(MonitoringQ3CompositionOwner& owner, core::Error value) noexcept {
        owner.seed_cleanup(std::move(value));
    }
    static void note_cleanup_overflow(MonitoringQ3CompositionOwner& owner) noexcept {
        owner.note_cleanup_overflow();
    }
};
} // namespace epycbench::monitoring::qualification::detail
