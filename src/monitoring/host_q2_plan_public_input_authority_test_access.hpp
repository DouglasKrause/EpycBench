#pragma once
#include "monitoring/host_q2_plan_public_input_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2PlanPublicInputAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ2PlanPublicInputAuthority::Operations;
    static auto retain(int root, const MonitoringQ1GitFixedResults& git, Operations operations) {
        return MonitoringQ2PlanPublicInputAuthority::retain_with(root, git, operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
