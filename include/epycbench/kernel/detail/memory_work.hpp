#pragma once

#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace epycbench::kernel::detail {
static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(std::endian::native == std::endian::little);

// Shared by the typed correctness-test entry and the standalone module entry.
// The inner memory work and compiler barriers are the original v0.1 algorithms.
[[gnu::always_inline]] inline auto sequential_read_work(
    const unsigned char* bytes, std::size_t word_count, std::uint64_t passes) noexcept
    -> std::uint64_t {
    std::uint64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, a7 = 0;
    asm volatile("" ::: "memory");
    do {
        for (std::size_t i = 0; i < word_count; i += 8U) {
            std::uint64_t value0, value1, value2, value3, value4, value5, value6, value7;
            std::memcpy(&value0, bytes + (i + 0U) * sizeof(std::uint64_t), sizeof(value0));
            std::memcpy(&value1, bytes + (i + 1U) * sizeof(std::uint64_t), sizeof(value1));
            std::memcpy(&value2, bytes + (i + 2U) * sizeof(std::uint64_t), sizeof(value2));
            std::memcpy(&value3, bytes + (i + 3U) * sizeof(std::uint64_t), sizeof(value3));
            std::memcpy(&value4, bytes + (i + 4U) * sizeof(std::uint64_t), sizeof(value4));
            std::memcpy(&value5, bytes + (i + 5U) * sizeof(std::uint64_t), sizeof(value5));
            std::memcpy(&value6, bytes + (i + 6U) * sizeof(std::uint64_t), sizeof(value6));
            std::memcpy(&value7, bytes + (i + 7U) * sizeof(std::uint64_t), sizeof(value7));
            a0 += value0; a1 += value1; a2 += value2; a3 += value3;
            a4 += value4; a5 += value5; a6 += value6; a7 += value7;
        }
        asm volatile("" ::: "memory");
        --passes;
    } while (passes != 0U);
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

[[gnu::always_inline]] inline auto dependent_load_work(
    const unsigned char* bytes, std::size_t start_index, std::uint64_t steps) noexcept
    -> std::uint64_t {
    std::size_t current_index = start_index;
    asm volatile("" ::: "memory");
    do {
        const auto* node_bytes = bytes + current_index * 64U;
        std::uint64_t next;
        std::memcpy(&next, node_bytes, sizeof(next));
        current_index = static_cast<std::size_t>(next);
        --steps;
    } while (steps != 0U);
    asm volatile("" ::: "memory");
    return current_index;
}
} // namespace epycbench::kernel::detail
