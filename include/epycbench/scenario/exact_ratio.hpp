#pragma once

#include "epycbench/core/error.hpp"

#include <compare>
#include <cstdint>
#include <expected>

namespace epycbench::scenario {

class ExactRatio {
  public:
    [[nodiscard]] static auto create(std::uint64_t numerator, std::uint64_t denominator)
        -> std::expected<ExactRatio, core::Error>;

    [[nodiscard]] auto numerator() const noexcept -> std::uint64_t { return numerator_; }
    [[nodiscard]] auto denominator() const noexcept -> std::uint64_t { return denominator_; }

    [[nodiscard]] auto operator<=>(const ExactRatio& other) const noexcept
        -> std::strong_ordering;
    auto operator==(const ExactRatio&) const -> bool = default;

  private:
    ExactRatio(std::uint64_t numerator, std::uint64_t denominator)
        : numerator_{numerator}, denominator_{denominator} {}

    std::uint64_t numerator_;
    std::uint64_t denominator_;
};

} // namespace epycbench::scenario
