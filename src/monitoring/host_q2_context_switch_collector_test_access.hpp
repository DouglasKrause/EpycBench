#pragma once
#include "monitoring/host_q2_context_switch_collector.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2CollectorTestAccess final {
  public:
    static auto collect(MonitoringQ2CollectorPlan&& plan,
                        MonitoringQ2CollectorOperations operations) noexcept {
        return MonitoringQ2ContextSwitchCollector::collect_with(std::move(plan), operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
