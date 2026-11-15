#pragma once

#include <cstdint>

namespace epycbench::kernel::detail {

[[nodiscard]] constexpr auto mix64_v1(std::uint64_t value) noexcept -> std::uint64_t {
    value += UINT64_C(0x9E3779B97F4A7C15);
    value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

} // namespace epycbench::kernel::detail
