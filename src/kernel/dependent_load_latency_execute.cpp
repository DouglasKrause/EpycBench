#include "epycbench/kernel/dependent_load_latency.hpp"

#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace epycbench::kernel {
namespace {

inline constexpr std::size_t node_size = 64U;

static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(std::endian::native == std::endian::little);

} // namespace

DependentLoadLatencyTerminalValue::DependentLoadLatencyTerminalValue(
    std::size_t value) noexcept
    : value_{value} {}

[[gnu::noinline]] auto execute_dependent_load_latency(
    const PreparedDependentLoadLatency& prepared,
    DependentLoadLatencySchedule schedule) noexcept
    -> DependentLoadLatencyTerminalValue {
    const auto bytes = prepared.input().logical_region();
    std::size_t current_index = schedule.start_index();
    auto remaining = schedule.step_count().value();

    asm volatile("" ::: "memory");
    do {
        const auto* node_bytes = bytes.data() + current_index * node_size;
        std::uint64_t next;
        std::memcpy(&next, node_bytes, sizeof(next));
        current_index = static_cast<std::size_t>(next);
        --remaining;
    } while (remaining != 0U);
    asm volatile("" ::: "memory");

    return DependentLoadLatencyTerminalValue{current_index};
}

} // namespace epycbench::kernel
