#pragma once

#include "monitoring/host_q4_cpufreq_prepared_source_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CpuFreqPreparedSourceAuthorityTestAccess final {
  public:
    using Operations = MonitoringQ4CpuFreqInventoryAuthority::Operations;
    using EnumerationState = MonitoringQ4CpuFreqEnumerationState;
    static auto prepare(const MonitoringQ4SourceReady& ready, core::LinuxCpuId cpu,
                        Operations operations) noexcept {
        return MonitoringQ4CpuFreqPreparedSourceAuthority::prepare_with(ready, cpu, operations);
    }
    static auto build_workspace(const MonitoringQ4SourceReady& ready, core::LinuxCpuId cpu,
                                Operations operations) noexcept {
        return MonitoringQ4CpuFreqPreparedSourceAuthority::build_workspace(ready, cpu, operations);
    }
    static auto execute_workspace(
        const MonitoringQ4SourceReady& ready,
        MonitoringQ4CpuFreqPreparedSourceAuthority::Workspace&& workspace) noexcept {
        return MonitoringQ4CpuFreqPreparedSourceAuthority::prepare_with_workspace(
            ready, std::move(workspace));
    }
    static void force_oracle_disagreement(MonitoringQ4CpuFreqPreparedSourceAuthority& value) {
        value.force_oracle_disagreement();
    }
    static auto parser_calls(const MonitoringQ4CpuFreqPreparedSourceAuthority& value) noexcept {
        return value.parser_calls();
    }
    static void force_parser_rejection(MonitoringQ4CpuFreqPreparedSourceAuthority& value,
                                       bool production, bool oracle) noexcept {
        value.force_parser_rejection(production, oracle);
    }
    static auto native_duplicate(int descriptor) noexcept -> int {
        const auto operations = MonitoringQ4CpuFreqInventoryAuthority::native_operations();
        return operations.duplicate(operations.context, descriptor);
    }
};
} // namespace epycbench::monitoring::qualification::detail
