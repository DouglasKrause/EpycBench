#pragma once
#include "monitoring/host_q4_cpufreq_collector.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CpuFreqCollectorTestAccess final {
  public:
    using Operations = MonitoringQ4CpuFreqCollector::Operations;
    static auto collect(const MonitoringQ4SourceReady& ready, Operations operations) noexcept {
        return MonitoringQ4CpuFreqCollector::collect_with(ready, operations);
    }
    static auto classify_cleanup_ledger_failure(MonitoringQ4CpuFreqCycleFailureKind kind) noexcept {
        return MonitoringQ4CpuFreqCollector::classify_cleanup_ledger_failure(kind);
    }
};
}
