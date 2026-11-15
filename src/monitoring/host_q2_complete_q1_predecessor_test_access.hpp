#pragma once
#include "monitoring/host_q2_complete_q1_predecessor_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2CompleteQ1PredecessorTestAccess final {
  public:
    static auto binding(const MonitoringQ1ValidatedPublicValues& values,std::string_view current,
                        std::string_view profile,std::string_view plan,
                        MonitoringQ2CompleteQ1PredecessorOperations operations) {
        return MonitoringQ2CompleteQ1PredecessorBinding{values,current,profile,plan,operations};
    }
    using SnapshotOperations=MonitoringQ2CompleteQ1PredecessorAuthority::SnapshotOperations;
    static auto retain(int root,const MonitoringQ2CompleteQ1PredecessorBinding& binding,SnapshotOperations operations){
        return MonitoringQ2CompleteQ1PredecessorAuthority::retain_with(root,binding,operations);
    }
    static void replace_operations(MonitoringQ2CompleteQ1PredecessorAuthority& authority,
                                   SnapshotOperations operations) noexcept {
        authority.snapshot_operations_=operations;
    }
    static auto retain_with_fault(const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                                  const MonitoringQ2CompleteQ1PredecessorBinding& binding,
                                  void* context,void (*fault)(void*,unsigned int)) {
        return MonitoringQ2CompleteQ1PredecessorAuthority::retain_with_fault(root,binding,context,fault);
    }
    static auto synthetic_active(std::array<int,11> descriptors,
      std::array<MonitoringQ2CompleteQ1PredecessorAuthority::SnapshotIdentity,11> identities,
      std::array<std::string,5> bytes,SnapshotOperations snapshot,
      MonitoringQ2CompleteQ1PredecessorOperations historical) {
        auto diagnostic=[](){return core::Error{.category=core::ErrorCategory::monitoring,
          .code=core::StableErrorCode{"monitoring.q2.test.synthetic"},.message="synthetic",
          .operation="synthetic_predecessor",.os_error=std::nullopt,.context={},
          .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};};
        std::array<std::optional<core::Error>,MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures> stock;
        for(auto& item:stock)item.emplace(diagnostic());
        auto value=MonitoringQ2CompleteQ1PredecessorAuthority{"historical","qid","current",
          "profile","plan","environment","qualification-plan","q0",
          core::sha256("manifest"),core::sha256("checksums"),core::sha256("q1"),historical,std::move(stock)};
        value.descriptors_=descriptors;value.identities_=identities;value.retained_bytes_=std::move(bytes);
        value.snapshot_operations_=snapshot;return value;
    }
};
}
