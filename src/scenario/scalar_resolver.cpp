#include "scenario/scalar_resolver.hpp"
#include "core/json_ownership.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace epycbench::scenario::detail {
namespace {

auto error(std::string code, std::string classification) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup cleanup{context};
    context["classification"] = std::move(classification);
    return core::Error{.category = core::ErrorCategory::scenario,
                       .code = core::StableErrorCode{std::move(code)},
                       .message = "Authored scalar is outside the restricted grammar",
                       .operation = "parse_authored_scenario",
                       .os_error = std::nullopt,
                       .context = std::move(context)};
}

auto digits(std::string_view text) -> bool {
    if (text.empty()) return false;
    for (const char value : text) {
        if (value < '0' || value > '9') return false;
    }
    return true;
}

auto integer_grammar(std::string_view text) -> bool {
    if (!text.empty() && text.front() == '-') text.remove_prefix(1);
    return text == "0" || (!text.empty() && text.front() >= '1' && text.front() <= '9' && digits(text));
}

auto float_grammar(std::string_view text) -> bool {
    if (!text.empty() && text.front() == '-') text.remove_prefix(1);
    const auto exponent = text.find_first_of("eE");
    auto mantissa = text.substr(0, exponent);
    if (exponent != std::string_view::npos) {
        auto exp = text.substr(exponent + 1);
        if (!exp.empty() && (exp.front() == '+' || exp.front() == '-')) exp.remove_prefix(1);
        if (!digits(exp)) return false;
    }
    const auto point = mantissa.find('.');
    if (point == std::string_view::npos) return exponent != std::string_view::npos && integer_grammar(mantissa);
    return integer_grammar(mantissa.substr(0, point)) && digits(mantissa.substr(point + 1));
}

auto nonfinite_attempt(std::string_view text) -> bool {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char value : text) {
        lowered.push_back(value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value);
    }
    return lowered == ".inf" || lowered == "+.inf" || lowered == "-.inf" || lowered == ".nan" ||
           lowered == "+.nan" || lowered == "-.nan";
}

} // namespace

auto resolve_plain_scalar(std::string_view text)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    if (text == "null") return nullptr;
    if (text == "true") return true;
    if (text == "false") return false;
    if (nonfinite_attempt(text)) {
        return std::unexpected{error("scenario.parse.nonfinite_number", "nonfinite_number")};
    }
    if (integer_grammar(text)) {
        if (text.front() == '-') {
            std::int64_t value{};
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
                return std::unexpected{error("scenario.parse.integer_out_of_range", "signed_integer")};
            }
            return value;
        }
        std::uint64_t value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
            return std::unexpected{error("scenario.parse.integer_out_of_range", "unsigned_integer")};
        }
        return value;
    }
    if (float_grammar(text)) {
        double value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value,
                                            std::chars_format::general);
        if (result.ec == std::errc::result_out_of_range) {
            const auto exponent = text.find_first_of("eE");
            if (exponent != std::string_view::npos && exponent + 1U < text.size() &&
                text[exponent + 1U] == '-') {
                return text.front() == '-' ? -0.0 : 0.0;
            }
        }
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(value)) {
            return std::unexpected{error("scenario.parse.nonfinite_number", "floating_point")};
        }
        return value;
    }
    return std::string{text};
}

} // namespace epycbench::scenario::detail
