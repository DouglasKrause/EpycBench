#include "scenario/byte_quantity_parser.hpp"

#include <cstdint>
#include <limits>

namespace epycbench::scenario::detail {
namespace {

auto fail(ByteQuantityFailure failure) -> std::unexpected<ByteQuantityError> {
    return std::unexpected{ByteQuantityError{failure}};
}

auto multiplier(std::string_view unit)
    -> std::expected<std::uint64_t, ByteQuantityError> {
    if (unit == "B") return 1U;
    if (unit == "KiB") return 1'024U;
    if (unit == "MiB") return 1'048'576U;
    if (unit == "GiB") return 1'073'741'824U;
    if (unit == "TiB") return 1'099'511'627'776U;
    if (unit == "kB") return 1'000U;
    if (unit == "MB") return 1'000'000U;
    if (unit == "GB") return 1'000'000'000U;
    if (unit == "TB") return 1'000'000'000'000U;
    return fail(ByteQuantityFailure::unsupported_unit);
}

} // namespace

auto parse_byte_quantity(std::string_view text)
    -> std::expected<core::ByteCount, ByteQuantityError> {
    if (!text.empty() && text.front() == '-')
        return fail(ByteQuantityFailure::negative_not_allowed);
    if (text.find('.') != std::string_view::npos)
        return fail(ByteQuantityFailure::fractional_not_allowed);

    const auto separator = text.find(' ');
    if (separator == std::string_view::npos || separator == 0U ||
        separator + 1U >= text.size() || text.find(' ', separator + 1U) != std::string_view::npos)
        return fail(ByteQuantityFailure::invalid_grammar);
    const auto number = text.substr(0U, separator);
    if (number.size() > 1U && number.front() == '0')
        return fail(ByteQuantityFailure::invalid_grammar);

    std::uint64_t value{};
    for (const char character : number) {
        if (character < '0' || character > '9')
            return fail(ByteQuantityFailure::invalid_grammar);
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            return fail(ByteQuantityFailure::overflow);
        value = value * 10U + digit;
    }
    auto factor = multiplier(text.substr(separator + 1U));
    if (!factor) return std::unexpected{factor.error()};
    if (value > std::numeric_limits<std::uint64_t>::max() / *factor)
        return fail(ByteQuantityFailure::overflow);
    return core::ByteCount::from_base_units(value * *factor);
}

auto byte_quantity_classification(ByteQuantityFailure failure) noexcept -> std::string_view {
    switch (failure) {
    case ByteQuantityFailure::invalid_grammar:
        return "invalid_grammar";
    case ByteQuantityFailure::unsupported_unit:
        return "unsupported_unit";
    case ByteQuantityFailure::negative_not_allowed:
        return "negative_not_allowed";
    case ByteQuantityFailure::fractional_not_allowed:
        return "fractional_not_allowed";
    case ByteQuantityFailure::overflow:
        return "overflow";
    }
    return "invalid_grammar";
}

} // namespace epycbench::scenario::detail
