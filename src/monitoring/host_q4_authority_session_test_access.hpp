#pragma once
#include "monitoring/host_q4_authority_session.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4AuthoritySessionTestAccess final {
  public:
    using AcquisitionOperations = MonitoringQ4AuthoritySession::AcquisitionOperations;
    using FinalizeOperations = MonitoringQ4AuthoritySession::FinalizeOperations;
    static auto acquire(AcquisitionOperations value) noexcept {
        return MonitoringQ4AuthoritySession::acquire_body(value);
    }
    static auto finalize(FinalizeOperations value) noexcept {
        return MonitoringQ4AuthoritySession::finalize_body(value);
    }
    static auto project(MonitoringQ4AuthorityFailure value) noexcept {
        return MonitoringQ4AuthoritySession::project_retired(std::move(value));
    }
    static auto empty(bool consumed = false) noexcept {
        return MonitoringQ4AuthoritySession{consumed};
    }
    static void bootstrap_fault(bool value) noexcept {
        MonitoringQ4AuthoritySession::set_bootstrap_fault(value);
    }
    static auto authenticate_controlled_normal(const std::filesystem::path& root,
        const MonitoringRunningBuildAuthority::Operations& operations) {
        return MonitoringQ4AuthoritySession::authenticate_impl(
            root, "MON-007.host-v2.campaign-20261005-01", false, &operations);
    }
    static auto normal_owner(MonitoringQ4AuthoritySession& session) noexcept
        -> MonitoringQ4CompositionOwner* {
        if (session.completed_mode_ || session.consumed_ || !session.owner_) return nullptr;
        return &*session.owner_;
    }
};
} // namespace epycbench::monitoring::qualification::detail
