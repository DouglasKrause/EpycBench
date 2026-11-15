#pragma once
#include "monitoring/host_q1_public_input_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1PublicInputAuthorityTestAccess final {
  public:
    static auto retain(int root, const MonitoringQ1GitFixedResults& git,
                       MonitoringQ1PublicInputAuthority::Operations operations) {
        return MonitoringQ1PublicInputAuthority::retain_with(root, git, operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
