#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace epycbench::kernel::detail {

// The reset path assigns a unique process-local identity so a reconstructed
// storage owner cannot accept an old trial at a reused virtual address.
inline auto next_counter_trial_generation() noexcept -> std::optional<std::uint64_t> {
    static std::atomic<std::uint64_t> next_generation{1U};
    auto next = next_generation.load(std::memory_order_relaxed);
    while (next != UINT64_MAX) {
        if (next_generation.compare_exchange_weak(next, next + 1U, std::memory_order_relaxed))
            return next;
    }
    return std::nullopt;
}
} // namespace epycbench::kernel::detail
