#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

namespace epycbench::core::detail {
static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53 &&
              std::numeric_limits<double>::max_exponent == 1024);

enum class NumberFailure { nonfinite, conversion };
struct NumberText {
    std::array<char, 32> bytes{};
    std::size_t size{};
    auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};

// Explicit integer rounding: no implementation-defined inexact integer-to-double cast.
inline auto integer_binary64(std::uint64_t magnitude, bool negative = false) noexcept -> double {
    if (magnitude == 0) {
        return 0.0;
    }
    unsigned exponent = 63U - static_cast<unsigned>(std::countl_zero(magnitude));
    std::uint64_t significand{};
    if (exponent <= 52) {
        significand = magnitude << (52U - exponent);
    } else {
        const auto shift = exponent - 52U;
        significand = magnitude >> shift;
        const auto remainder = magnitude & ((std::uint64_t{1} << shift) - 1);
        const auto halfway = std::uint64_t{1} << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (significand & 1U) != 0)) {
            ++significand;
            if (significand == (std::uint64_t{1} << 53U)) {
                significand >>= 1U;
                ++exponent;
            }
        }
    }
    const auto bits = (negative ? std::uint64_t{1} << 63U : 0) |
                      (static_cast<std::uint64_t>(exponent + 1023U) << 52U) |
                      (significand & ((std::uint64_t{1} << 52U) - 1));
    return std::bit_cast<double>(bits);
}
inline auto integer_binary64(std::int64_t value) noexcept -> double {
    const bool negative = value < 0;
    const auto unsigned_value = static_cast<std::uint64_t>(value);
    const auto magnitude = negative ? std::uint64_t{0} - unsigned_value : unsigned_value;
    return integer_binary64(magnitude, negative);
}

inline auto number_text(double value) noexcept -> std::expected<NumberText, NumberFailure> {
    if (!std::isfinite(value)) {
        return std::unexpected{NumberFailure::nonfinite};
    }
    NumberText result;
    auto append = [&](char c) { result.bytes[result.size++] = c; };
    if (value == 0) {
        append('0');
        return result;
    }
    if (std::signbit(value)) {
        append('-');
        value = -value;
    }
    std::array<char, 32> scientific{};
    const auto converted = std::to_chars(scientific.data(), scientific.data() + scientific.size(),
                                         value, std::chars_format::scientific);
    if (converted.ec != std::errc{}) {
        return std::unexpected{NumberFailure::conversion};
    }
    const std::string_view text{scientific.data(),
                                static_cast<std::size_t>(converted.ptr - scientific.data())};
    const auto e = text.find('e');
    if (e == std::string_view::npos) {
        return std::unexpected{NumberFailure::conversion};
    }
    std::array<char, 17> digits{};
    int k{};
    for (std::size_t i = 0; i < e; ++i) {
        if (text[i] != '.') {
            if (k == static_cast<int>(digits.size())) {
                return std::unexpected{NumberFailure::conversion};
            }
            digits[static_cast<std::size_t>(k++)] = text[i];
        }
    }
    const auto exponent_text = text.substr(e + 1);
    int exponent{};
    const auto skip = !exponent_text.empty() && exponent_text.front() == '+' ? 1U : 0U;
    const auto parsed = std::from_chars(exponent_text.data() + skip,
                                        exponent_text.data() + exponent_text.size(), exponent);
    if (parsed.ec != std::errc{} || parsed.ptr != exponent_text.data() + exponent_text.size() ||
        k == 0) {
        return std::unexpected{NumberFailure::conversion};
    }
    const int n = exponent + 1;
    if (k <= n && n <= 21) {
        for (int i = 0; i < k; ++i) {
            append(digits[static_cast<std::size_t>(i)]);
        }
        for (int i = k; i < n; ++i) {
            append('0');
        }
    } else if (0 < n && n <= 21) {
        for (int i = 0; i < k; ++i) {
            if (i == n) {
                append('.');
            }
            append(digits[static_cast<std::size_t>(i)]);
        }
    } else if (-6 < n && n <= 0) {
        append('0');
        append('.');
        for (int i = 0; i < -n; ++i) {
            append('0');
        }
        for (int i = 0; i < k; ++i) {
            append(digits[static_cast<std::size_t>(i)]);
        }
    } else {
        append(digits[0]);
        if (k > 1) {
            append('.');
        }
        for (int i = 1; i < k; ++i) {
            append(digits[static_cast<std::size_t>(i)]);
        }
        append('e');
        if (exponent >= 0) {
            append('+');
        }
        const auto rendered = std::to_chars(result.bytes.data() + result.size,
                                            result.bytes.data() + result.bytes.size(), exponent);
        if (rendered.ec != std::errc{}) {
            return std::unexpected{NumberFailure::conversion};
        }
        result.size = static_cast<std::size_t>(rendered.ptr - result.bytes.data());
    }
    return result;
}
} // namespace epycbench::core::detail
