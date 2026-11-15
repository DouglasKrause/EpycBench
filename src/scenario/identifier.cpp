#include "epycbench/scenario/identifier.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace epycbench::scenario {
namespace {

auto error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.identifier.invalid_text"},
            .message = "Scenario identifier text is invalid",
            .operation = "parse_scenario_identifier",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"failure_classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

constexpr auto lowercase_ascii(char value) noexcept -> bool {
    return value >= 'a' && value <= 'z';
}

constexpr auto subsequent_ascii(char value) noexcept -> bool {
    return lowercase_ascii(value) || (value >= '0' && value <= '9') || value == '-';
}

constexpr auto invalid_encoding(char value) noexcept -> bool {
    return value == '\0' || static_cast<unsigned char>(value) >= 0x80U;
}

} // namespace

auto ScenarioIdentifier::parse(std::string_view text)
    -> std::expected<ScenarioIdentifier, core::Error> {
    try {
        if (text.empty())
            return std::unexpected{error("empty")};
        if (text.size() > 64U)
            return std::unexpected{error("too_long")};
        if (invalid_encoding(text.front()))
            return std::unexpected{error("invalid_encoding")};
        if (!lowercase_ascii(text.front()))
            return std::unexpected{error("invalid_first_character")};
        for (std::size_t index = 1U; index < text.size(); ++index) {
            if (invalid_encoding(text[index]))
                return std::unexpected{error("invalid_encoding")};
            if (!subsequent_ascii(text[index]))
                return std::unexpected{error("invalid_character")};
        }
        return ScenarioIdentifier{std::string{text}};
    } catch (const std::exception&) {
        return std::unexpected{error("invalid_encoding")};
    } catch (...) {
        return std::unexpected{error("invalid_encoding")};
    }
}

} // namespace epycbench::scenario
