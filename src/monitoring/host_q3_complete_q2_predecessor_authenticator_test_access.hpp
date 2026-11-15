#pragma once
#include "monitoring/host_q3_complete_q2_predecessor_authenticator.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3CompleteQ2PredecessorAuthenticatorTestAccess final {
  public:
    static auto authenticate_campaign(MonitoringQ3CompleteQ2PredecessorInput input,
        MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations operations,
        const MonitoringQ1ValidatedPublicValues& values) {
        return MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_campaign(
            std::move(input), operations, values);
    }
    static auto authenticate(MonitoringQ3CompleteQ2PredecessorInput input,
                             MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations operations,
                             const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests& expected) {
        return MonitoringQ3CompleteQ2PredecessorAuthenticator::authenticate_with(
            std::move(input), operations, expected);
    }
};
} // namespace epycbench::monitoring::qualification::detail
