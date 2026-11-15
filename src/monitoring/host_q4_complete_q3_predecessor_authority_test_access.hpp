#pragma once
#include "monitoring/host_q4_complete_q3_predecessor_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CompleteQ3PredecessorAuthorityTestAccess final {
  public:
    static auto capture_campaign(int root, const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ1ValidatedPublicValues& values,
        MonitoringQ4CompleteQ3PredecessorAuthority::Operations operations) {
        return MonitoringQ4CompleteQ3PredecessorAuthority::capture_with(root, candidate, values, operations);
    }
    static auto bind_campaign(MonitoringQ4CompleteQ3PredecessorAuthority& snapshot,
        const MonitoringQ4HistoricalGitValue& history) { return snapshot.bind_campaign(history); }
    static auto provisional_commits(const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot) {
        return snapshot.provisional_commits_;
    }
    static auto history(const MonitoringQ4CompleteQ3PredecessorAuthority& value)
        -> const MonitoringQ4HistoricalGitValue& {
        return *value.history_;
    }
    static auto mutable_history(MonitoringQ4CompleteQ3PredecessorAuthority& value)
        -> MonitoringQ4HistoricalGitValue& {
        return *value.history_;
    }
    static auto production_inventory(int descriptor) {
        return MonitoringQ4CompleteQ3PredecessorAuthority::production_inventory(nullptr, descriptor);
    }
    using Operations = MonitoringQ4CompleteQ3PredecessorAuthority::Operations;
    using ProjectionOperations = MonitoringQ4CompleteQ3PredecessorAuthority::ProjectionOperations;
    static auto retain(int root, const MonitoringQ4GovernedInputCandidate& candidate,
                       const MonitoringQ4HistoricalGitValue& history, Operations operations,
                       const auto& expected) {
        return MonitoringQ4CompleteQ3PredecessorAuthority::retain_with(root, candidate, history,
                                                                       operations, &expected);
    }
    static auto active(const MonitoringQ4CompleteQ3PredecessorAuthority& value) {
        return value.active_;
    }
    static auto make_active_for_retirement(
        Operations operations,
        std::array<int, MonitoringQ4CompleteQ3PredecessorAuthority::descriptor_count> descriptors) {
        MonitoringQ4CompleteQ3PredecessorAuthority value;
        value.operations_ = operations;
        value.descriptors_ = descriptors;
        value.active_.set();
        return value;
    }
    static auto project(const MonitoringQ4CompleteQ3AuthorityFailure& failure,
                        ProjectionOperations operations) {
        return MonitoringQ4CompleteQ3PredecessorAuthority::project_failure_with(failure,
                                                                                operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
