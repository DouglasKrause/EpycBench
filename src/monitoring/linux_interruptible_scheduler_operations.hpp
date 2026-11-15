#pragma once

#include "epycbench/monitoring/linux_interruptible_scheduler.hpp"

namespace epycbench::monitoring {

struct detail_LinuxInterruptibleSchedulerAccess {
    using Operations = LinuxInterruptibleRawMonotonicScheduler::Operations;

    [[nodiscard]] static auto create(Operations operations) noexcept
        -> LinuxInterruptibleRawMonotonicScheduler {
        return LinuxInterruptibleRawMonotonicScheduler{operations};
    }

    [[nodiscard]] static auto production_clock_bound(
        const LinuxInterruptibleRawMonotonicScheduler& scheduler) noexcept -> bool {
        return scheduler.production_clock_;
    }
};

} // namespace epycbench::monitoring
