#pragma once
#include "monitoring/host_q2_complete_q1_predecessor_authority.hpp"
#include "monitoring/host_q1_git_fixed_results.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2HistoricalQ1GitAuthority final {
  public:
    [[nodiscard]] static auto authenticate(
      const MonitoringQ1GitExecutable&,const core::detail::OwnedGovernedRepositoryRootDescriptor&,
      const MonitoringQ1GitRepositoryMetadata&,std::string_view historical,std::string_view current,
      std::string_view q0,std::string_view profile,std::string_view q1_plan)
      noexcept -> std::expected<void,MonitoringQ2HistoricalFailure>;
  private:
    struct TestOperations final { void* context{}; void (*fault)(void*,unsigned int){}; };
    static auto authenticate_with(const MonitoringQ1GitExecutable&,const core::detail::OwnedGovernedRepositoryRootDescriptor&,
      const MonitoringQ1GitRepositoryMetadata&,std::string_view,std::string_view,std::string_view,std::string_view,std::string_view,
      TestOperations) noexcept->std::expected<void,MonitoringQ2HistoricalFailure>;
    static auto transport_for_test(MonitoringQ1GitCommandFailure) noexcept->MonitoringQ2HistoricalFailure;
    static auto command_for_test(void*, MonitoringQ1GitCommandFailure (*)(void*)) noexcept
      ->MonitoringQ2HistoricalFailure;
    friend class MonitoringQ2HistoricalQ1GitAuthorityTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
