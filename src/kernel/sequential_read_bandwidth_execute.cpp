#include "epycbench/kernel/sequential_read_bandwidth.hpp"

#include <bit>
#include <climits>
#include <cstdint>
#include <cstring>

namespace epycbench::kernel {
namespace {

static_assert(CHAR_BIT == 8);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(std::endian::native == std::endian::little);

} // namespace

SequentialReadBandwidthTerminalValue::SequentialReadBandwidthTerminalValue(
    std::uint64_t value) noexcept
    : value_{value} {}

[[gnu::noinline]] auto execute_sequential_read_bandwidth(
    const PreparedSequentialReadBandwidth& prepared,
    SequentialReadBandwidthPassCount passes) noexcept
    -> SequentialReadBandwidthTerminalValue {
    std::uint64_t a0 = 0;
    std::uint64_t a1 = 0;
    std::uint64_t a2 = 0;
    std::uint64_t a3 = 0;
    std::uint64_t a4 = 0;
    std::uint64_t a5 = 0;
    std::uint64_t a6 = 0;
    std::uint64_t a7 = 0;

    const auto bytes = prepared.input().logical_region();
    const auto word_count = prepared.word_count();
    auto remaining_passes = passes.value();

    asm volatile("" ::: "memory");
    do {
        for (std::size_t i = 0; i < word_count; i += 8U) {
            std::uint64_t value0;
            std::uint64_t value1;
            std::uint64_t value2;
            std::uint64_t value3;
            std::uint64_t value4;
            std::uint64_t value5;
            std::uint64_t value6;
            std::uint64_t value7;

            std::memcpy(&value0,
                        bytes.data() + (i + 0U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value1,
                        bytes.data() + (i + 1U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value2,
                        bytes.data() + (i + 2U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value3,
                        bytes.data() + (i + 3U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value4,
                        bytes.data() + (i + 4U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value5,
                        bytes.data() + (i + 5U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value6,
                        bytes.data() + (i + 6U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));
            std::memcpy(&value7,
                        bytes.data() + (i + 7U) * sizeof(std::uint64_t),
                        sizeof(std::uint64_t));

            a0 += value0;
            a1 += value1;
            a2 += value2;
            a3 += value3;
            a4 += value4;
            a5 += value5;
            a6 += value6;
            a7 += value7;
        }
        asm volatile("" ::: "memory");
        --remaining_passes;
    } while (remaining_passes != 0U);

    return SequentialReadBandwidthTerminalValue{a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7};
}

} // namespace epycbench::kernel
