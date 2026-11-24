#pragma once

#include <atomic>
#include <cstdint>

namespace epycbench::kernel::detail {

inline void increment_atomic_counter(std::uint64_t& destination, std::uint64_t count) noexcept {
    static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
    const std::atomic_ref<std::uint64_t> counter{destination};
    for (std::uint64_t index{}; index < count; ++index)
        counter.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace epycbench::kernel::detail
