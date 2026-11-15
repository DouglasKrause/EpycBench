#pragma once

#include "epycbench/runtime/runner_control_monitor.hpp"

namespace epycbench::runtime::detail {

struct RunnerControlMonitorAccess {
    [[nodiscard]] static auto cancellation_code_valid(std::uint8_t code) noexcept -> bool {
        return RunnerCancellation::decode(static_cast<RunnerCancellation::Code>(code))
                   .has_value() ||
               code == static_cast<std::uint8_t>(RunnerCancellation::Code::none);
    }

    [[nodiscard]] static auto
    active_destruction_is_fatal(const RunnerControlMonitor& monitor) noexcept -> bool {
        return monitor.destroying_active_is_fatal();
    }
};

} // namespace epycbench::runtime::detail
