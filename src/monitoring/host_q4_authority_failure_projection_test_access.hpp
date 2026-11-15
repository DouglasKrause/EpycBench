#pragma once
#include "monitoring/host_q4_authority_failure_projection.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4AuthorityFailureProjectorTestAccess final {
  public:
    static auto project(MonitoringQ4AuthorityFailure value) noexcept {
        return MonitoringQ4AuthorityFailureProjector::project(std::move(value));
    }
    static void fault(unsigned char value) noexcept {
        MonitoringQ4AuthorityFailureProjector::set_fault(value);
    }
};
} // namespace epycbench::monitoring::qualification::detail
