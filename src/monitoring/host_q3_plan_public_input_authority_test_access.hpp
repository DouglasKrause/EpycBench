#pragma once
#include "monitoring/host_q3_plan_public_input_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3PlanPublicInputAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ3PlanPublicInputAuthority::Operations;
    static auto retain(int root, const MonitoringQ1GitFixedResults& git, Operations operations) {
        return MonitoringQ3PlanPublicInputAuthority::retain_with(root, git, operations);
    }
    static auto retain_with_decoder(int root, const MonitoringQ1GitFixedResults& git,
        Operations operations,
        std::expected<std::shared_ptr<const void>, MonitoringGovernedPlanDecodeFailure>
            (*decoder)(std::string_view)) {
        return MonitoringQ3PlanPublicInputAuthority::retain_with_decoder(
            root, git, operations, decoder);
    }
};
} // namespace epycbench::monitoring::qualification::detail
