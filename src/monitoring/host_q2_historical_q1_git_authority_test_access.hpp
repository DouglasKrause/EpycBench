#pragma once
#include "monitoring/host_q2_historical_q1_git_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2HistoricalQ1GitAuthorityTestAccess final { public:
 static auto transport(MonitoringQ1GitCommandFailure value) noexcept{return MonitoringQ2HistoricalQ1GitAuthority::transport_for_test(std::move(value));}
 static auto command(void* context,MonitoringQ1GitCommandFailure (*operation)(void*)) noexcept{return MonitoringQ2HistoricalQ1GitAuthority::command_for_test(context,operation);}
 static auto authenticate(const MonitoringQ1GitExecutable& executable,const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
  const MonitoringQ1GitRepositoryMetadata& metadata,std::string_view historical,std::string_view current,std::string_view q0,std::string_view profile,
  std::string_view plan,void* context,void (*fault)(void*,unsigned int)) noexcept {
   return MonitoringQ2HistoricalQ1GitAuthority::authenticate_with(executable,root,metadata,historical,current,q0,profile,plan,{context,fault});}
};}
