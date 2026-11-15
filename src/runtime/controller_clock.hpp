#pragma once

#include <chrono>

namespace epycbench::runtime::detail {

using ControllerTimePoint = std::chrono::steady_clock::time_point;

struct ControllerClock {
    void* context;
    ControllerTimePoint (*now)(void* context) noexcept;
};

[[nodiscard]] auto production_controller_clock() noexcept -> ControllerClock;

} // namespace epycbench::runtime::detail
