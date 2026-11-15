#pragma once
#include "monitoring/host_running_build_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringRunningBuildAuthorityTestAccess final { public:
 using Operations=MonitoringRunningBuildAuthority::Operations;
 static auto retain(std::string_view commit,Operations operations) noexcept {
  return MonitoringRunningBuildAuthority::retain_with(commit,operations);}
};}
