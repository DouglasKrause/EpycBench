#pragma once

#include "epycbench/platform_linux/hardware_counters.hpp"

#include <span>

namespace epycbench::platform_linux::hardware_counter_detail {

[[nodiscard]] auto decode(std::span<const std::uint64_t> words, std::uint64_t cycles_id,
                          std::uint64_t instructions_id) -> std::expected<HardwareCounterValues, core::Error>;

[[nodiscard]] auto subtract(const HardwareCounterValues& initial, const HardwareCounterValues& final)
    -> std::expected<HardwareCounterValues, core::Error>;

} // namespace epycbench::platform_linux::hardware_counter_detail
