#pragma once

#include "epycbench/core/quantity.hpp"

#include <compare>
#include <expected>
#include <string_view>

namespace epycbench::scenario::detail {

enum class ByteQuantityFailure {
    invalid_grammar,
    unsupported_unit,
    negative_not_allowed,
    fractional_not_allowed,
    overflow,
};

struct ByteQuantityError {
    ByteQuantityFailure failure;
    auto operator<=>(const ByteQuantityError&) const = default;
};

[[nodiscard]] auto parse_byte_quantity(std::string_view text)
    -> std::expected<core::ByteCount, ByteQuantityError>;
[[nodiscard]] auto byte_quantity_classification(ByteQuantityFailure failure) noexcept
    -> std::string_view;

} // namespace epycbench::scenario::detail
