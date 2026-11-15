#pragma once

#include "monitoring/host_q4_authority_session.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4AuthorityFailureProjectorTestAccess;

class MonitoringQ4AuthorityFailureProjector final {
  private:
    static auto project(MonitoringQ4AuthorityFailure) noexcept
        -> MonitoringQ4ProjectedAuthorityFailure;
    static void set_fault(unsigned char) noexcept;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4AuthorityFailureProjectorTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
