#include "epycbench/scenario/exact_ratio.hpp"

#include <numeric>

namespace epycbench::scenario {
namespace {

auto compare_fractions(std::uint64_t left_numerator, std::uint64_t left_denominator,
                       std::uint64_t right_numerator, std::uint64_t right_denominator) noexcept
    -> std::strong_ordering {
    const auto maybe_invert = [](std::strong_ordering result, bool invert) {
        if (!invert || result == std::strong_ordering::equal) return result;
        return result == std::strong_ordering::less ? std::strong_ordering::greater
                                                    : std::strong_ordering::less;
    };
    bool inverted = false;
    while (true) {
        const auto left_quotient = left_numerator / left_denominator;
        const auto right_quotient = right_numerator / right_denominator;
        if (left_quotient != right_quotient) {
            const auto result = left_quotient <=> right_quotient;
            return maybe_invert(result, inverted);
        }

        const auto left_remainder = left_numerator % left_denominator;
        const auto right_remainder = right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            const auto result = left_remainder <=> right_remainder;
            return maybe_invert(result, inverted);
        }

        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        inverted = !inverted;
    }
}

} // namespace

auto ExactRatio::create(std::uint64_t numerator, std::uint64_t denominator)
    -> std::expected<ExactRatio, core::Error> {
    if (denominator == 0U || numerator > denominator) {
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.exact_ratio.invalid"},
            .message = "Exact ratio must have a positive denominator and lie in [0,1]",
            .operation = "ExactRatio::create",
            .os_error = std::nullopt,
            .context = nlohmann::json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command,
        }};
    }
    const auto divisor = std::gcd(numerator, denominator);
    return ExactRatio{numerator / divisor, denominator / divisor};
}

auto ExactRatio::operator<=>(const ExactRatio& other) const noexcept -> std::strong_ordering {
    return compare_fractions(numerator_, denominator_, other.numerator_, other.denominator_);
}

} // namespace epycbench::scenario
