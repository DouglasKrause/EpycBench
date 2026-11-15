#pragma once

#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include <expected>
#include <time.h>

namespace epycbench::platform_linux::detail {

struct RawClockResult {
    int native_result;
    int error_number;
    timespec timestamp;
};

struct ClockOperations {
    void* context;
    RawClockResult (*clock_gettime_raw)(void*, clockid_t) noexcept;
};

[[nodiscard]] auto production_clock_operations() noexcept -> ClockOperations;

class RawMonotonicClockAccess {
  public:
    [[nodiscard]] static auto now(const ClockOperations& operations) noexcept
        -> std::expected<RawMonotonicTimestamp, core::Error>;
};

} // namespace epycbench::platform_linux::detail
