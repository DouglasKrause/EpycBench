#pragma once

#include "kernel/mix64.hpp"

#include <bit>
#include <cstddef>
#include <span>

namespace epycbench::kernel::detail {
// Only bounded numerators and reduction sums in [-2^35, 2^35] use this encoding.
inline auto dyadic_numerator(std::uint64_t seed, std::uint64_t index) noexcept -> std::int32_t {
    return static_cast<std::int32_t>(mix64_v1(seed + index) & UINT64_C(65535)) - 32768;
}
inline auto dyadic_magnitude(std::int64_t value) noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(value < 0 ? -value : value);
}
inline auto dyadic_magnitude_bits(std::uint64_t value) noexcept -> std::uint64_t {
    if (value == 0U) return 0U;
    const auto top = static_cast<unsigned>(std::bit_width(value)) - 1U;
    const auto exponent = top + (1023U - 15U);
    return (static_cast<std::uint64_t>(exponent) << 52U) |
        ((value << (52U - top)) & UINT64_C(0x000fffffffffffff));
}
inline auto dyadic_bits(std::int64_t value) noexcept -> std::uint64_t {
    return dyadic_magnitude_bits(dyadic_magnitude(value)) | (value < 0 ? UINT64_C(0x8000000000000000) : 0U);
}
inline void write_dyadic_word(std::span<std::byte> bytes, std::size_t offset, std::uint64_t word) noexcept {
    for (unsigned byte{}; byte < 8U; ++byte)
        bytes[offset + byte] = static_cast<std::byte>((word >> (byte * 8U)) & 0xffU);
}
inline auto read_dyadic_word(std::span<const std::byte> bytes, std::size_t offset) noexcept -> std::uint64_t {
    std::uint64_t word{};
    for (unsigned byte{}; byte < 8U; ++byte)
        word |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + byte])} << (byte * 8U);
    return word;
}
} // namespace epycbench::kernel::detail
