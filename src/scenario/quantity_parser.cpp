#include "scenario/quantity_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string_view>

namespace epycbench::scenario::detail {
namespace {

struct Decimal {
    std::uint64_t coefficient;
    std::size_t scale;
};

constexpr auto error(QuantityParseFailure failure, std::string_view classification)
    -> std::unexpected<QuantityParseError> {
    return std::unexpected{QuantityParseError{failure, classification}};
}

auto parse_decimal(std::string_view text) -> std::expected<Decimal, QuantityParseError> {
    if (text.empty())
        return error(QuantityParseFailure::invalid_quantity, "empty_number");

    const auto point = text.find('.');
    if (point != std::string_view::npos && text.find('.', point + 1) != std::string_view::npos) {
        return error(QuantityParseFailure::invalid_quantity, "multiple_decimal_points");
    }

    const auto integer_end = point == std::string_view::npos ? text.size() : point;
    if (integer_end == 0 || (point != std::string_view::npos && point + 1 == text.size())) {
        return error(QuantityParseFailure::invalid_quantity, "missing_decimal_digits");
    }
    if (text[0] == '0' && integer_end > 1) {
        return error(QuantityParseFailure::invalid_quantity, "leading_zero");
    }

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == point)
            continue;
        if (text[index] < '0' || text[index] > '9') {
            return error(QuantityParseFailure::invalid_quantity, "non_ascii_digit");
        }
    }

    std::size_t significant_end = text.size();
    if (point != std::string_view::npos) {
        while (significant_end > point + 1 && text[significant_end - 1] == '0')
            --significant_end;
    }
    const auto scale = point == std::string_view::npos ? 0U : significant_end - point - 1U;

    std::uint64_t coefficient{};
    for (std::size_t index = 0; index < significant_end; ++index) {
        if (index == point)
            continue;
        const auto digit = static_cast<std::uint64_t>(text[index] - '0');
        if (coefficient > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return error(QuantityParseFailure::overflow, "coefficient_overflow");
        }
        coefficient = coefficient * 10U + digit;
    }
    return Decimal{.coefficient = coefficient, .scale = scale};
}

auto power_of_ten(std::size_t scale) -> std::expected<std::uint64_t, QuantityParseError> {
    std::uint64_t value = 1;
    for (std::size_t index = 0; index < scale; ++index) {
        if (value > std::numeric_limits<std::uint64_t>::max() / 10U) {
            return error(QuantityParseFailure::out_of_range, "precision_limit");
        }
        value *= 10U;
    }
    return value;
}

auto duration_multiplier(std::string_view unit)
    -> std::expected<std::uint64_t, QuantityParseError> {
    if (unit == "ns")
        return 1U;
    if (unit == "us")
        return 1'000U;
    if (unit == "ms")
        return 1'000'000U;
    if (unit == "s")
        return 1'000'000'000U;
    if (unit == "min")
        return 60'000'000'000U;
    return error(QuantityParseFailure::unsupported_unit, "duration_unit");
}

auto split_duration(std::string_view text)
    -> std::expected<std::pair<std::string_view, std::string_view>, QuantityParseError> {
    const auto separator = text.find(' ');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size() ||
        text.find(' ', separator + 1) != std::string_view::npos ||
        text.find('\t') != std::string_view::npos) {
        return error(QuantityParseFailure::invalid_quantity, "duration_framing");
    }
    return std::pair{text.substr(0, separator), text.substr(separator + 1)};
}

} // namespace

auto make_exact_ratio(std::uint64_t numerator, std::uint64_t denominator)
    -> std::expected<ExactRatio, QuantityParseError> {
    auto ratio = ExactRatio::create(numerator, denominator);
    if (!ratio) {
        return error(QuantityParseFailure::out_of_range, "ratio_invariant");
    }
    return std::move(*ratio);
}

auto parse_duration(std::string_view text) -> std::expected<core::Duration, QuantityParseError> {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const auto parts = split_duration(text);
    if (!parts)
        return std::unexpected{parts.error()};
    const auto multiplier = duration_multiplier(parts->second);
    if (!multiplier)
        return std::unexpected{multiplier.error()};
    const auto decimal = parse_decimal(parts->first);
    if (!decimal) {
        if (negative && decimal.error().failure == QuantityParseFailure::overflow) {
            return error(QuantityParseFailure::negative_not_allowed, "negative_duration");
        }
        return std::unexpected{decimal.error()};
    }
    if (negative)
        return error(QuantityParseFailure::negative_not_allowed, "negative_duration");

    const auto denominator = power_of_ten(decimal->scale);
    if (!denominator) {
        return error(QuantityParseFailure::inexact_conversion, "sub_nanosecond_precision");
    }

    const auto common = std::gcd(*denominator, *multiplier);
    const auto remaining_denominator = *denominator / common;
    const auto reduced_multiplier = *multiplier / common;
    if (decimal->coefficient % remaining_denominator != 0) {
        return error(QuantityParseFailure::inexact_conversion, "sub_nanosecond_value");
    }
    const auto reduced_coefficient = decimal->coefficient / remaining_denominator;
    if (reduced_multiplier != 0 &&
        reduced_coefficient > std::numeric_limits<std::uint64_t>::max() / reduced_multiplier) {
        return error(QuantityParseFailure::overflow, "nanosecond_overflow");
    }
    return core::Duration::from_base_units(reduced_coefficient * reduced_multiplier);
}

auto parse_percentage(std::string_view text)
    -> std::expected<ExactRatio, QuantityParseError> {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    if (text.size() < 2 || text.back() != '%') {
        return error(QuantityParseFailure::invalid_quantity, "percentage_framing");
    }
    text.remove_suffix(1);
    const auto decimal = parse_decimal(text);
    if (!decimal) {
        if (negative && decimal.error().failure == QuantityParseFailure::overflow) {
            return error(QuantityParseFailure::negative_not_allowed, "negative_percentage");
        }
        return std::unexpected{decimal.error()};
    }
    if (negative)
        return error(QuantityParseFailure::negative_not_allowed, "negative_percentage");
    if (decimal->scale > 17) {
        return error(QuantityParseFailure::out_of_range, "precision_limit");
    }
    const auto scale_denominator = power_of_ten(decimal->scale);
    if (!scale_denominator)
        return std::unexpected{scale_denominator.error()};
    if (decimal->coefficient > 100U * *scale_denominator) {
        return error(QuantityParseFailure::out_of_range, "percentage_range");
    }
    const auto denominator = *scale_denominator * 100U;
    return make_exact_ratio(decimal->coefficient, denominator);
}

} // namespace epycbench::scenario::detail
