#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace epycbench::statistics::detail {

class SplitMix64 {
  public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_{seed} {}

    [[nodiscard]] constexpr auto next() noexcept -> std::uint64_t {
        state_ += UINT64_C(0x9E3779B97F4A7C15);
        auto value = state_;
        value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
        value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
        return value ^ (value >> 31U);
    }

  private:
    std::uint64_t state_;
};

template <class NextWord>
[[nodiscard]] constexpr auto unbiased_index(std::uint64_t population_size,
                                            NextWord&& next_word) noexcept -> std::uint64_t {
    const std::uint64_t threshold = (std::uint64_t{0} - population_size) % population_size;
    std::uint64_t word{};
    do {
        word = next_word();
    } while (word < threshold);
    return word % population_size;
}

[[nodiscard]] auto type7_quantile(std::span<const double> sorted, double probability)
    -> std::expected<double, core::Error>;

} // namespace epycbench::statistics::detail
