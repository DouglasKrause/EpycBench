#include "epycbench/core/canonical_json.hpp"
#include "jcs_number.hpp"
#include "json_ownership.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::core {
namespace {

enum class UnicodeFailure { invalid_utf8, invalid_scalar };

struct DecodedString {
    std::vector<std::uint32_t> scalars;
};

auto unicode_error(UnicodeFailure failure, std::string path, std::string classification) -> Error {
    auto context = nlohmann::json::object();
    detail::JsonCleanup cleanup{context};
    context["path"] = std::move(path);
    context["classification"] = std::move(classification);
    const auto scalar = failure == UnicodeFailure::invalid_scalar;
    return Error{
        .category = ErrorCategory::core,
        .code = StableErrorCode{scalar ? "core.canonical_json.invalid_unicode_scalar"
                                       : "core.canonical_json.invalid_utf8"},
        .message = scalar ? "JSON text contains an encoded Unicode surrogate"
                          : "JSON text is not well-formed UTF-8",
        .operation = "canonicalize_json",
        .os_error = std::nullopt,
        .context = std::move(context),
        .severity = ErrorSeverity::error,
        .continuation = ContinuationPolicy::abort_command,
    };
}

auto canonical_error(std::string code, std::string message, std::string path,
                     std::string classification) -> Error {
    auto context = nlohmann::json::object();
    detail::JsonCleanup cleanup{context};
    context["path"] = std::move(path);
    context["classification"] = std::move(classification);
    return Error{
        .category = ErrorCategory::core,
        .code = StableErrorCode{std::move(code)},
        .message = std::move(message),
        .operation = "canonicalize_json",
        .os_error = std::nullopt,
        .context = std::move(context),
        .severity = ErrorSeverity::error,
        .continuation = ContinuationPolicy::abort_command,
    };
}

auto decode_utf8(std::string_view text, bool retain = true) -> std::expected<DecodedString, UnicodeFailure> {
    DecodedString decoded;
    if (retain) { decoded.scalars.reserve(text.size()); }
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        std::uint32_t scalar{};
        std::size_t length{};
        std::uint32_t minimum{};
        if (first <= 0x7fU) {
            scalar = first;
            length = 1;
            minimum = 0;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            scalar = first & 0x1fU;
            length = 2;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            scalar = first & 0x0fU;
            length = 3;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            scalar = first & 0x07U;
            length = 4;
            minimum = 0x10000U;
        } else {
            return std::unexpected{UnicodeFailure::invalid_utf8};
        }
        if (length > text.size() - index) {
            return std::unexpected{UnicodeFailure::invalid_utf8};
        }
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto byte = static_cast<std::uint8_t>(text[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) {
                return std::unexpected{UnicodeFailure::invalid_utf8};
            }
            scalar = (scalar << 6U) | (byte & 0x3fU);
        }
        if (scalar < minimum || scalar > 0x10ffffU) {
            return std::unexpected{UnicodeFailure::invalid_utf8};
        }
        if (scalar >= 0xd800U && scalar <= 0xdfffU) {
            return std::unexpected{UnicodeFailure::invalid_scalar};
        }
        if (retain) { decoded.scalars.push_back(scalar); }
        index += length;
    }
    return decoded;
}

auto utf16_units(const DecodedString& decoded) -> std::vector<std::uint16_t> {
    std::vector<std::uint16_t> result;
    result.reserve(decoded.scalars.size());
    for (const auto scalar : decoded.scalars) {
        if (scalar <= 0xffffU) {
            result.push_back(static_cast<std::uint16_t>(scalar));
        } else {
            const auto adjusted = scalar - 0x10000U;
            result.push_back(static_cast<std::uint16_t>(0xd800U + (adjusted >> 10U)));
            result.push_back(static_cast<std::uint16_t>(0xdc00U + (adjusted & 0x3ffU)));
        }
    }
    return result;
}

void append_escaped_string(std::string_view text, const DecodedString& decoded, std::string& output) {
    constexpr std::string_view hex{"0123456789abcdef"};
    output.push_back('"');
    std::size_t byte_index = 0;
    for (const auto scalar : decoded.scalars) {
        if (scalar == '"') {
            output += "\\\"";
        } else if (scalar == '\\') {
            output += "\\\\";
        } else if (scalar == '\b') {
            output += "\\b";
        } else if (scalar == '\t') {
            output += "\\t";
        } else if (scalar == '\n') {
            output += "\\n";
        } else if (scalar == '\f') {
            output += "\\f";
        } else if (scalar == '\r') {
            output += "\\r";
        } else if (scalar <= 0x1fU) {
            output += "\\u00";
            output.push_back(hex[(scalar >> 4U) & 0x0fU]);
            output.push_back(hex[scalar & 0x0fU]);
        } else {
            std::size_t byte_count = 1;
            if (scalar >= 0x80U) {
                byte_count = scalar < 0x800U ? 2U : (scalar < 0x10000U ? 3U : 4U);
            }
            output.append(text.substr(byte_index, byte_count));
        }
        if (scalar < 0x80U) {
            byte_index += 1U;
        } else if (scalar < 0x800U) {
            byte_index += 2U;
        } else if (scalar < 0x10000U) {
            byte_index += 3U;
        } else {
            byte_index += 4U;
        }
    }
    output.push_back('"');
}

auto child_path(std::string_view parent, std::string_view key, const DecodedString& decoded)
    -> std::string {
    std::string quoted;
    append_escaped_string(key, decoded, quoted);
    return std::string{parent} + '[' + quoted + ']';
}

auto jcs_error(std::string_view classification) -> Error {
    return {.category = ErrorCategory::core,
            .code = StableErrorCode{"core.canonical_json_jcs." + std::string{classification}},
            .message = "JCS input rejected",
            .operation = "canonicalize_json_jcs",
            .os_error = std::nullopt,
            .context = nlohmann::json::object(),
            .severity = ErrorSeverity::error,
            .continuation = ContinuationPolicy::abort_command};
}
auto jcs_number(const nlohmann::json& value) noexcept
    -> std::expected<detail::NumberText, detail::NumberFailure> {
    if (value.is_number_unsigned()) {
        return detail::number_text(detail::integer_binary64(value.get<std::uint64_t>()));
    }
    if (value.is_number_integer()) {
        return detail::number_text(detail::integer_binary64(value.get<std::int64_t>()));
    }
    return detail::number_text(value.get<double>());
}

auto append_value(const nlohmann::json& value, std::string_view path, std::string& output,
                  bool jcs = false) -> std::expected<void, Error> {
    if (jcs && value.is_number()) {
        const auto text = jcs_number(value);
        if (!text) {
            return std::unexpected{jcs_error(
                text.error() == detail::NumberFailure::nonfinite ? "nonfinite" : "conversion")};
        }
        output += text->view();
        return {};
    }
    switch (value.type()) {
    case nlohmann::json::value_t::null:
        output += "null";
        return {};
    case nlohmann::json::value_t::boolean:
        output += value.get<bool>() ? "true" : "false";
        return {};
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned: {
        std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 3U> buffer{};
        std::to_chars_result converted{};
        if (value.is_number_unsigned()) {
            converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                      value.get<std::uint64_t>());
        } else {
            converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                      value.get<std::int64_t>());
        }
        output.append(buffer.data(), converted.ptr);
        return {};
    }
    case nlohmann::json::value_t::number_float:
        return std::unexpected{canonical_error(
            "core.canonical_json.floating_point_unsupported",
            "Floating-point JSON values are unsupported by the canonical profile",
            std::string{path}, "floating_point_unsupported")};
    case nlohmann::json::value_t::string: {
        const auto& text = value.get_ref<const std::string&>();
        const auto decoded = decode_utf8(text);
        if (!decoded) {
            return std::unexpected{unicode_error(decoded.error(), std::string{path},
                                                 "string_value")};
        }
        append_escaped_string(text, *decoded, output);
        return {};
    }
    case nlohmann::json::value_t::array: {
        output.push_back('[');
        for (std::size_t index = 0; index < value.size(); ++index) {
            if (index != 0U) {
                output.push_back(',');
            }
            auto result = append_value(
                value[index], std::string{path} + '[' + std::to_string(index) + ']', output, jcs);
            if (!result) {
                return std::unexpected{std::move(result.error())};
            }
        }
        output.push_back(']');
        return {};
    }
    case nlohmann::json::value_t::object: {
        struct Member {
            const std::string* key;
            const nlohmann::json* value;
            DecodedString decoded;
            std::vector<std::uint16_t> ordering_key;
        };
        std::vector<Member> members;
        members.reserve(value.size());
        for (auto iterator = value.cbegin(); iterator != value.cend(); ++iterator) {
            auto decoded = decode_utf8(iterator.key());
            if (!decoded) {
                return std::unexpected{unicode_error(decoded.error(), std::string{path},
                                                     "object_property_name")};
            }
            auto units = utf16_units(*decoded);
            members.push_back(Member{&iterator.key(), &iterator.value(), std::move(*decoded),
                                     std::move(units)});
        }
        std::ranges::sort(members, [](const Member& left, const Member& right) {
            return std::lexicographical_compare(left.ordering_key.begin(), left.ordering_key.end(),
                                                right.ordering_key.begin(), right.ordering_key.end());
        });
        output.push_back('{');
        for (std::size_t index = 0; index < members.size(); ++index) {
            if (index != 0U) {
                output.push_back(',');
            }
            const auto& member = members[index];
            append_escaped_string(*member.key, member.decoded, output);
            output.push_back(':');
            auto result = append_value(*member.value, child_path(path, *member.key, member.decoded),
                                       output, jcs);
            if (!result) {
                return std::unexpected{std::move(result.error())};
            }
        }
        output.push_back('}');
        return {};
    }
    case nlohmann::json::value_t::binary:
    case nlohmann::json::value_t::discarded:
        return std::unexpected{canonical_error(
            "core.canonical_json.unsupported_type",
            "The JSON value type is unsupported by the canonical profile", std::string{path},
            "unsupported_type")};
    }
    return std::unexpected{canonical_error(
        "core.canonical_json.unsupported_type",
        "The JSON value type is unsupported by the canonical profile", std::string{path},
        "unsupported_type")};
}

// No input copy or proportional workspace is allocated by this preflight.
auto preflight_jcs(const nlohmann::json& root, JcsLimits limits)
    -> std::expected<std::size_t, Error> {
    using Json = nlohmann::json;
    if (limits.depth > 64 || limits.output_bytes > std::string{}.max_size()) {
        return std::unexpected{jcs_error("limits")};
    }
    struct Frame {
        const Json* value;
        Json::const_iterator next;
        bool entered;
    };
    std::array<Frame, 64> frames{};
    frames[0] = {&root, {}, false};
    std::size_t used = 1, nodes = 0, bytes = 0;
    auto add = [&](std::size_t amount) {
        if (amount > limits.output_bytes - bytes) {
            return false;
        }
        bytes += amount;
        return true;
    };
    auto string_size = [&](std::string_view text) -> std::expected<void, Error> {
        if (!add(2)) {
            return std::unexpected{jcs_error("limits")};
        }
        for (char byte : text) {
            const auto c = static_cast<unsigned char>(byte);
            const std::size_t width = c == '"' || c == '\\' || c == '\b' || c == '\f' ||
                                              c == '\n' || c == '\r' || c == '\t'
                                          ? 2U
                                      : c < 0x20 ? 6U
                                                 : 1U;
            if (!add(width)) {
                return std::unexpected{jcs_error("limits")};
            }
        }
        if (!decode_utf8(text, false)) {
            return std::unexpected{jcs_error("unicode")};
        }
        return {};
    };
    while (used != 0) {
        auto& frame = frames[used - 1];
        const auto& value = *frame.value;
        if (!frame.entered) {
            if (used > limits.depth || nodes == limits.nodes) {
                return std::unexpected{jcs_error("limits")};
            }
            ++nodes;
            frame.entered = true;
            if (value.is_structured() && !value.is_binary()) {
                if (!add(2)) {
                    return std::unexpected{jcs_error("limits")};
                }
                frame.next = value.begin();
            } else {
                if (value.is_string()) {
                    auto ok = string_size(value.get_ref<const std::string&>());
                    if (!ok) {
                        return std::unexpected{ok.error()};
                    }
                } else if (value.is_number()) {
                    const auto text = jcs_number(value);
                    if (!text) {
                        return std::unexpected{jcs_error(
                            text.error() == detail::NumberFailure::nonfinite ? "nonfinite"
                                                                             : "conversion")};
                    }
                    if (!add(text->size)) {
                        return std::unexpected{jcs_error("limits")};
                    }
                } else if (value.is_null() || value.is_boolean()) {
                    if (!add(value.is_null() || value.get<bool>() ? 4U : 5U)) {
                        return std::unexpected{jcs_error("limits")};
                    }
                } else {
                    return std::unexpected{jcs_error("unsupported_type")};
                }
                --used;
                continue;
            }
        }
        if (frame.next == value.end()) {
            --used;
            continue;
        }
        if (frame.next != value.begin() && !add(1)) {
            return std::unexpected{jcs_error("limits")};
        }
        if (value.is_object()) {
            auto ok = string_size(frame.next.key());
            if (!ok) {
                return std::unexpected{ok.error()};
            }
            if (!add(1)) {
                return std::unexpected{jcs_error("limits")};
            }
        }
        if (used == frames.size()) {
            return std::unexpected{jcs_error("limits")};
        }
        const auto* child = &*frame.next++;
        frames[used++] = {child, {}, false};
    }
    return bytes;
}

} // namespace

auto canonicalize_json(const nlohmann::json& value) -> std::expected<std::string, Error> {
    std::string output;
    auto result = append_value(value, "$", output);
    if (!result) {
        return std::unexpected{std::move(result.error())};
    }
    return output;
}

auto hash_canonical_json(const nlohmann::json& value)
    -> std::expected<Sha256Digest, Error> {
    auto canonical = canonicalize_json(value);
    if (!canonical) {
        return std::unexpected{std::move(canonical.error())};
    }
    return sha256(*canonical);
}

auto canonicalize_json_jcs(const nlohmann::json& value, JcsLimits limits)
    -> std::expected<std::string, Error> {
    const auto size = preflight_jcs(value, limits);
    if (!size) {
        return std::unexpected{size.error()};
    }
    std::string output;
    output.reserve(*size);
    auto result = append_value(value, "$", output, true);
    if (!result) {
        detail::JsonCleanup error_cleanup{result.error().context};
        return std::unexpected{jcs_error("conversion")};
    }
    return output;
}
auto hash_canonical_json_jcs(const nlohmann::json& value, JcsLimits limits)
    -> std::expected<Sha256Digest, Error> {
    auto canonical = canonicalize_json_jcs(value, limits);
    if (!canonical) {
        return std::unexpected{std::move(canonical.error())};
    }
    return sha256(*canonical);
}

} // namespace epycbench::core
