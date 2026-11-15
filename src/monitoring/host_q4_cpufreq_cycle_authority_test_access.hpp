#pragma once
#include "monitoring/host_q4_cpufreq_cycle_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CpuFreqCycleAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ4CpuFreqCycleAuthority::Operations;
    static auto execute(const MonitoringQ4SourceReady& ready,
                        MonitoringQ4CpuFreqCleanupLedger& ledger, Operations operations) noexcept {
        return MonitoringQ4CpuFreqCycleAuthority::execute_with(ready, ledger, operations);
    }
    static auto create(const MonitoringQ4SourceReady& ready,
                       MonitoringQ4CpuFreqCleanupLedger& ledger, Operations operations) noexcept {
        return MonitoringQ4CpuFreqCycleAuthority::create_with(ready, ledger, operations);
    }
    static auto cleanup_capacity(const MonitoringQ4CpuFreqCleanupLedger& value) noexcept {
        return value.capacity();
    }
    static void append_cleanup(MonitoringQ4CpuFreqCleanupLedger& value,
                               std::size_t count) noexcept {
        for (std::size_t index{}; index < count; ++index)
            MonitoringQ4CpuFreqCycleAuthority::observe_cleanup(
                value.storage_.get(),
                static_cast<unsigned>(MonitoringQ4CpuFreqCycleCleanupRole::root), 0, 0, 0, false);
    }
    static void append_cleanup_event(MonitoringQ4CpuFreqCleanupLedger& value,
                                     MonitoringQ4CpuFreqCycleCleanupRole role, std::uint64_t policy,
                                     std::uint64_t attribute, int native, bool uncertain) noexcept {
        MonitoringQ4CpuFreqCycleAuthority::observe_cleanup(value.storage_.get(),
                                                           static_cast<unsigned>(role), policy,
                                                           attribute, native, uncertain);
    }
    static auto cleanup_size(const MonitoringQ4CpuFreqCleanupLedger& value) noexcept {
        return value.events().size();
    }
    static auto cleanup_overflowed(const MonitoringQ4CpuFreqCleanupLedger& value) noexcept {
        return value.overflowed();
    }
    static void replace_initial_domain(MonitoringQ4CpuFreqCycleAuthority& value,
                                       std::size_t admitted_index,
                                       std::uint64_t domain_ordinal) noexcept {
        value.initial_->cpus_[admitted_index].domain_ordinal = domain_ordinal;
    }
};
} // namespace epycbench::monitoring::qualification::detail
