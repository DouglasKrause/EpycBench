#pragma once
#include "monitoring/host_q3_historical_git_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3HistoricalGitAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ3HistoricalGitAuthority::Operations;
    static auto authenticate(Operations operations, std::string_view q0, std::string_view q1,
                             std::string_view q2, std::string_view current,
                             std::string_view profile, std::string_view q1_plan,
                             std::string_view q2_plan, std::string_view q3_plan) {
        return MonitoringQ3HistoricalGitAuthority::authenticate_with(
            operations, q0, q1, q2, current, profile, q1_plan, q2_plan, q3_plan);
    }
};
} // namespace epycbench::monitoring::qualification::detail
