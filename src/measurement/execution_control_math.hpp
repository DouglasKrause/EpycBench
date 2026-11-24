#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace epycbench::measurement::detail {

inline constexpr std::size_t maximum_calibration_probes = 128U;

[[nodiscard]] constexpr auto calibration_distance(std::uint64_t value,
                                                   std::uint64_t target) noexcept
    -> std::uint64_t {
    return value < target ? target - value : value - target;
}

[[nodiscard]] constexpr auto calibration_double(std::uint64_t count, std::uint64_t ceiling) noexcept
    -> std::uint64_t {
    return count > ceiling - count ? ceiling : count * 2U;
}

[[nodiscard]] constexpr auto calibration_midpoint(std::uint64_t lower, std::uint64_t upper) noexcept
    -> std::uint64_t {
    return lower + (upper - lower) / 2U;
}

[[nodiscard]] inline auto rational_less_equal(
    std::uint64_t left_numerator, std::uint64_t left_denominator,
    std::uint64_t right_numerator, std::uint64_t right_denominator) noexcept -> bool {
    bool reversed = false;
    while (true) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient)
            return reversed ? left_quotient > right_quotient
                            : left_quotient < right_quotient;
        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            if (left_remainder == 0U && right_remainder == 0U)
                return true;
            return reversed ? right_remainder == 0U : left_remainder == 0U;
        }
        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        reversed = !reversed;
    }
}

struct ElapsedWindow {
    std::uint64_t median;
    std::uint64_t maximum_deviation;
    bool stable;
};

[[nodiscard]] inline auto evaluate_elapsed_window_control(std::span<std::uint64_t> elapsed,
                                                          std::uint64_t numerator,
                                                          std::uint64_t denominator) noexcept -> ElapsedWindow {
    std::ranges::sort(elapsed);
    const auto median = elapsed[(elapsed.size() - 1U) / 2U];
    const auto deviation = std::max(calibration_distance(elapsed.front(), median),
                                    calibration_distance(elapsed.back(), median));
    return {median, deviation, median == 0U ? deviation == 0U :
        rational_less_equal(deviation, median, numerator, denominator)};
}

} // namespace epycbench::measurement::detail
