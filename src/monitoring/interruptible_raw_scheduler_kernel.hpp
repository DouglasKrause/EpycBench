#pragma once

#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <utility>

namespace epycbench::monitoring::detail {

// Positive authored intervals remain the classifier's worst-case gap limit.
// Point reads need headroom for their actual nonzero acquisition envelopes.
[[nodiscard]] constexpr auto point_acquisition_period_nanoseconds(
    std::uint64_t coverage_interval) noexcept -> std::uint64_t {
    return coverage_interval > 1U ? coverage_interval / 2U : 1U;
}

enum class InterruptibleRawWaitResult : unsigned char { deadline, interrupted };

template <class Now, class Block>
[[nodiscard]] auto wait_until_raw_monotonic(
    std::mutex& mutex, std::condition_variable& condition, bool& pending_wake,
    platform_linux::RawMonotonicTimestamp deadline, Now&& now,
    Block&& block) noexcept
    -> std::expected<InterruptibleRawWaitResult,
                     typename decltype(now())::error_type> {
    std::unique_lock lock{mutex};
    for (;;) {
        if (std::exchange(pending_wake, false))
            return InterruptibleRawWaitResult::interrupted;
        auto current = now();
        if (!current)
            return std::unexpected{std::move(current.error())};
        if (*current >= deadline)
            return InterruptibleRawWaitResult::deadline;
        using Rep = std::chrono::nanoseconds::rep;
        constexpr auto maximum_block =
            static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
        const auto remaining = deadline.nanoseconds() - current->nanoseconds();
        block(condition, lock,
              remaining > maximum_block ? maximum_block : remaining);
    }
}

inline void wake_raw_monotonic(std::mutex& mutex,
                               std::condition_variable& condition,
                               bool& pending_wake) noexcept {
    {
        std::lock_guard lock{mutex};
        pending_wake = true;
    }
    condition.notify_one();
}

} // namespace epycbench::monitoring::detail
