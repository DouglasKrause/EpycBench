#pragma once

#include "monitoring/host_q3_complete_q2_predecessor_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3CompleteQ2PredecessorAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ3CompleteQ2PredecessorAuthority::Operations;
    static auto binding(std::string_view qualification_id, std::string_view current_commit,
        std::string_view profile, std::string_view q1_plan, std::string_view q2_plan,
        std::string_view q3_plan, MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history) {
        return MonitoringQ3CompleteQ2PredecessorBinding{qualification_id, current_commit, profile,
                                                        q1_plan, q2_plan, q3_plan, history};
    }
    static auto campaign_binding(std::string_view qualification_id, std::string_view current_commit,
        std::string_view profile, std::string_view q1_plan, std::string_view q2_plan,
        std::string_view q3_plan, MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history,
        const MonitoringQ1ValidatedPublicValues& values) {
        return MonitoringQ3CompleteQ2PredecessorBinding{qualification_id,current_commit,profile,
            q1_plan,q2_plan,q3_plan,history,&values};
    }
    static auto retain(int root, const MonitoringQ3CompleteQ2PredecessorBinding& binding,
        Operations operations,
        const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests& expected) {
        return MonitoringQ3CompleteQ2PredecessorAuthority::retain_with(root, binding, operations, &expected);
    }
    static auto retain_native(int root, const MonitoringQ3CompleteQ2PredecessorBinding& binding,
        const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests& expected) {
        return MonitoringQ3CompleteQ2PredecessorAuthority::retain_with_native_expected(
            root, binding, expected);
    }
    static auto retain_native_fault(int root,
        const MonitoringQ3CompleteQ2PredecessorBinding& binding,
        const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests& expected,
        void* context, void (*fault)(void*, unsigned int)) {
        return MonitoringQ3CompleteQ2PredecessorAuthority::retain_with_native_fault(
            root, binding, expected, context, fault);
    }
    static void replace_operations(MonitoringQ3CompleteQ2PredecessorAuthority& authority,
                                   Operations operations) noexcept {
        authority.operations_ = operations;
    }
    static auto synthetic_active(std::array<int, 13> descriptors, Operations operations) {
        MonitoringQ3CompleteQ2PredecessorAuthority authority;
        authority.descriptors_ = descriptors;
        authority.operations_ = operations;
        authority.active_ = true;
        return authority;
    }
};
} // namespace epycbench::monitoring::qualification::detail
