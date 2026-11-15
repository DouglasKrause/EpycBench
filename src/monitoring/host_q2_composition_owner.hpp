#pragma once
#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include <memory>
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2PlanPublicInputAuthority;class MonitoringQ2CompleteQ1PredecessorAuthority;
class MonitoringRunningBuildAuthority;
struct MonitoringQ2CompositionFailure final {
  core::Error primary;
  MonitoringQ1BoundedRetirement cleanup;
};
class MonitoringQ2CompositionOwner final {
 public:
  static auto create() noexcept->std::expected<MonitoringQ2CompositionOwner,MonitoringQ1GitCommandFailure>;
  MonitoringQ2CompositionOwner(MonitoringQ2CompositionOwner&&) noexcept;
  MonitoringQ2CompositionOwner(const MonitoringQ2CompositionOwner&)=delete;
  ~MonitoringQ2CompositionOwner() noexcept;
  void adopt_q1(MonitoringQ1PlanV2AuthoritySession) noexcept;
  auto q1() noexcept->MonitoringQ1PlanV2AuthoritySession&;
  auto take_q1() noexcept->MonitoringQ1PlanV2AuthoritySession;
  void adopt_input(MonitoringQ2PlanPublicInputAuthority) noexcept;
  void adopt_predecessor(MonitoringQ2CompleteQ1PredecessorAuthority) noexcept;
  void adopt_build(MonitoringRunningBuildAuthority) noexcept;
  auto input() noexcept->MonitoringQ2PlanPublicInputAuthority&;
  auto predecessor() noexcept->MonitoringQ2CompleteQ1PredecessorAuthority&;
  auto build() noexcept->MonitoringRunningBuildAuthority&;
  auto take_input() noexcept->MonitoringQ2PlanPublicInputAuthority;
  auto take_predecessor() noexcept->MonitoringQ2CompleteQ1PredecessorAuthority;
  auto take_build() noexcept->MonitoringRunningBuildAuthority;
  auto take_cleanup_ledger() noexcept->MonitoringQ1BoundedRetirement;
  [[nodiscard]] auto owns_q1() const noexcept->bool;
  auto rollback(core::Error) noexcept->MonitoringQ2CompositionFailure;
  auto rollback(MonitoringQ1GitCommandFailure) noexcept->MonitoringQ2CompositionFailure;
 private:
  void adopt_q1_test(void*,void (*)(void*) noexcept) noexcept;
  class Impl;
  explicit MonitoringQ2CompositionOwner(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class MonitoringQ2CompositionOwnerTestAccess;
};
class MonitoringQ2CompositionOwnerTestAccess final { public:
 static void adopt_q1(MonitoringQ2CompositionOwner& owner,void* context,void (*retire)(void*) noexcept){owner.adopt_q1_test(context,retire);}
 static void seed_cleanup(MonitoringQ2CompositionOwner&,core::Error) noexcept;
 static void note_cleanup_overflow(MonitoringQ2CompositionOwner&) noexcept;
};
}
