#include "epycbench/scenario/parser.hpp"
#include "core/json_ownership.hpp"

#include "scenario/json_cleanup.hpp"
#include "scenario/restricted_yaml_parser.hpp"
#include "scenario/strict_json_parser.hpp"

#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::scenario {
namespace {

constexpr std::size_t max_document = 16U << 20U;
constexpr std::size_t max_source_name = 4'096;

auto error(std::string code, std::string classification, ScenarioFormat format) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["format"] = format == ScenarioFormat::yaml ? "yaml" : "json";
    context["classification"] = std::move(classification);
    return core::Error{.category = core::ErrorCategory::scenario,
                       .code = core::StableErrorCode{std::move(code)},
                       .message = "Authored document violates the parser boundary",
                       .operation = "parse_authored_scenario",
                       .os_error = std::nullopt,
                       .context = std::move(context)};
}

auto valid_utf8(std::string_view text) -> bool {
    std::size_t index{};
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t value{};
        std::size_t count{};
        if (lead <= 0x7fU) { value = lead; count = 1; }
        else if (lead >= 0xc2U && lead <= 0xdfU) { value = lead & 0x1fU; count = 2; }
        else if (lead >= 0xe0U && lead <= 0xefU) { value = lead & 0x0fU; count = 3; }
        else if (lead >= 0xf0U && lead <= 0xf4U) { value = lead & 0x07U; count = 4; }
        else return false;
        if (index + count > text.size()) return false;
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (continuation & 0x3fU);
        }
        if ((count == 3 && value < 0x800U) || (count == 4 && value < 0x10000U) ||
            (value >= 0xd800U && value <= 0xdfffU) || value > 0x10ffffU) return false;
        index += count;
    }
    return true;
}

} // namespace

AuthoredScenario::AuthoredScenario(nlohmann::ordered_json value,
                                   std::map<std::string, NodeLocations> locations,
                                   std::string source_name)
    : locations_(std::move(locations)), source_name_(std::move(source_name)),
      value_(std::move(value)) {}

AuthoredScenario::AuthoredScenario(const AuthoredScenario& source)
    : locations_(source.locations_), source_name_(source.source_name_),
      value_(detail::clone_json(source.value_)) {}
AuthoredScenario::~AuthoredScenario() { detail::retire_json(value_); }
auto AuthoredScenario::operator=(AuthoredScenario&& source) noexcept -> AuthoredScenario& {
    if (this != &source) {
        detail::retire_json(value_);
        locations_ = std::move(source.locations_);
        source_name_ = std::move(source.source_name_);
        value_ = std::move(source.value_);
    }
    return *this;
}
auto AuthoredScenario::operator=(const AuthoredScenario& source) -> AuthoredScenario& {
    AuthoredScenario copy{source};
    return *this = std::move(copy);
}

auto make_authored_scenario(nlohmann::ordered_json value,
                            std::map<std::string, NodeLocations> locations,
                            std::string source_name) -> AuthoredScenario {
    return AuthoredScenario{std::move(value), std::move(locations), std::move(source_name)};
}

auto parse_authored_scenario(const ScenarioSource& source)
    -> std::expected<AuthoredScenario, core::Error> {
    try {
        if (source.logical_name.size() > max_source_name)
            return std::unexpected{error("scenario.parse.source_name_too_large", "source_name_bytes", source.format)};
        if (source.contents.size() > max_document)
            return std::unexpected{error("scenario.parse.document_too_large", "document_bytes", source.format)};
        if (source.contents.find('\0') != std::string_view::npos)
            return std::unexpected{error("scenario.parse.embedded_nul", "embedded_nul", source.format)};
        if (!valid_utf8(source.contents))
            return std::unexpected{error("scenario.parse.invalid_utf8", "invalid_utf8", source.format)};
        for (const char raw_value : source.contents) {
            const auto value = static_cast<unsigned char>(raw_value);
            if (value < 0x20U && value != '\n' && value != '\r' && value != '\t')
                return std::unexpected{error("scenario.parse.invalid_scalar", "raw_control_character", source.format)};
        }

        if (source.format == ScenarioFormat::yaml) {
            auto parsed = detail::parse_restricted_yaml(source.contents);
            core::detail::JsonPointerCleanup<nlohmann::json> parsed_error_cleanup{
                parsed ? nullptr : &parsed.error().context};
            if (!parsed)
                return std::unexpected{std::move(parsed.error())};
            detail::JsonCleanup cleanup{parsed->value};
            std::string name{source.logical_name};
            return make_authored_scenario(std::move(parsed->value), std::move(parsed->locations),
                                          std::move(name));
        }

        auto marked = detail::parse_restricted_yaml(source.contents, true);
        core::detail::JsonPointerCleanup<nlohmann::json> marked_error_cleanup{
            marked ? nullptr : &marked.error().context};
        nlohmann::ordered_json empty_marked;
        detail::JsonCleanup marked_cleanup{marked ? marked->value : empty_marked};
        auto value = detail::parse_strict_json(source.contents);
        core::detail::JsonPointerCleanup<nlohmann::json> value_error_cleanup{
            value ? nullptr : &value.error().context};
        nlohmann::ordered_json empty_value;
        detail::JsonCleanup value_cleanup{value ? *value : empty_value};
        if (!value) {
            if (!marked && value.error().code.value() == "scenario.parse.duplicate_key" &&
                marked.error().code.value() == "scenario.parse.duplicate_key") {
                marked.error().context["format"] = "json";
                return std::unexpected{std::move(marked.error())};
            }
            return std::unexpected{std::move(value.error())};
        }
        if (!marked) {
            return std::unexpected{error("scenario.parse.internal_parser_failure", "json_location_mismatch",
                                         source.format)};
        }
        auto verified = detail::verify_json_location_value(*value, marked->value);
        core::detail::JsonPointerCleanup<nlohmann::json> verified_error_cleanup{
            verified ? nullptr : &verified.error().context};
        if (!verified)
            return std::unexpected{std::move(verified.error())};
        std::string name{source.logical_name};
        return make_authored_scenario(std::move(*value), std::move(marked->locations),
                                      std::move(name));
    } catch (const std::exception&) {
        return std::unexpected{error("scenario.parse.internal_parser_failure", "contained_exception", source.format)};
    } catch (...) {
        return std::unexpected{error("scenario.parse.internal_parser_failure", "contained_unknown_failure",
                                     source.format)};
    }
}

} // namespace epycbench::scenario
