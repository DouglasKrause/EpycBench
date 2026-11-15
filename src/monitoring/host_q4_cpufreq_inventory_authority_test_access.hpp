#pragma once

#include "monitoring/host_q4_cpufreq_inventory_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CpuFreqInventoryAuthorityTestAccess final {
  public:
    static auto prepare_only(const MonitoringQ4CollectionCapacityInputs& inputs) noexcept {
        return MonitoringQ4CpuFreqInventoryAuthority::prepare_workspace(inputs, {});
    }

    using Operations = MonitoringQ4CpuFreqInventoryAuthority::Operations;
    using EnumerationState = MonitoringQ4CpuFreqEnumerationState;
    static auto retain(const MonitoringQ4CollectionCapacityInputs& inputs,
                       Operations operations) noexcept {
        return MonitoringQ4CpuFreqInventoryAuthority::retain_with(inputs, operations);
    }
    static void limit_cleanup(MonitoringQ4CpuFreqInventoryAuthority& value, std::size_t capacity) {
        value.storage_.cleanup.clear();
        value.storage_.cleanup.shrink_to_fit();
        value.storage_.cleanup.reserve(capacity);
        value.storage_.cleanup_limit = capacity;
    }
};
} // namespace epycbench::monitoring::qualification::detail
