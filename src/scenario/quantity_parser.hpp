#pragma once

#include "epycbench/core/quantity.hpp"
#include "epycbench/scenario/exact_ratio.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <string_view>

namespace epycbench::scenario::detail {

enum class QuantityParseFailure {
    invalid_quantity,
    unsupported_unit,
    negative_not_allowed,
    inexact_conversion,
    out_of_range,
    overflow,
};

struct QuantityParseError {
    QuantityParseFailure failure;
    std::string_view classification;

    auto operator<=>(const QuantityParseError&) const = default;
};

[[nodiscard]] auto make_exact_ratio(std::uint64_t numerator, std::uint64_t denominator)
    -> std::expected<ExactRatio, QuantityParseError>;

[[nodiscard]] auto parse_duration(std::string_view text)
    -> std::expected<core::Duration, QuantityParseError>;

[[nodiscard]] auto parse_percentage(std::string_view text)
    -> std::expected<ExactRatio, QuantityParseError>;

} // namespace epycbench::scenario::detail
