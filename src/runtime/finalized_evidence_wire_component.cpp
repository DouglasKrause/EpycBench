#include "runtime/finalized_evidence_wire_component.hpp"

namespace epycbench::runtime::wire_v1::detail {
namespace {
auto whitespace(char c) noexcept -> bool {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
auto delimiter(char c) noexcept -> bool {
    return whitespace(c) || c == ',' || c == ':' || c == '[' || c == ']' || c == '{' || c == '}';
}
}

auto count_component_wire_shape(std::string_view text, std::size_t depth_limit) noexcept
    -> std::expected<ComponentWireShape, WireJsonError> {
    ComponentWireShape result{};
    result.bytes = text.size();
    std::array<char, 64> parents{};
    std::size_t containers{};
    auto increment = [](std::size_t& value, std::size_t amount = 1U) noexcept {
        const auto sum = add_wire_json_count(value, amount);
        if (!sum) return false;
        value = *sum; return true;
    };
    auto node = [&]() noexcept {
        if (!increment(result.nodes)) return false;
        if (containers && parents[containers - 1U] == '[' && !increment(result.elements))
            return false;
        result.depth = std::max(result.depth, containers + 1U);
        return true;
    };
    for (std::size_t i{}; i < text.size();) {
        const auto c = text[i];
        if (whitespace(c) || c == ',' || c == ':') { ++i; continue; }
        if (c == '}' || c == ']') {
            if (!containers || parents[containers - 1U] != (c == '}' ? '{' : '['))
                return std::unexpected{WireJsonError{WireJsonFailure::malformed}};
            --containers; ++i; continue;
        }
        if (c == '"') {
            const auto begin = i++;
            bool ended{};
            while (i < text.size()) {
                if (text[i] == '\\') {
                    if (++i == text.size()) break;
                    ++i;
                } else if (text[i++] == '"') { ended = true; break; }
            }
            if (!ended) return std::unexpected{WireJsonError{WireJsonFailure::malformed}};
            if (!increment(result.strings, i - begin))
                return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
            auto after = i;
            while (after < text.size() && whitespace(text[after])) ++after;
            if (after < text.size() && text[after] == ':') {
                if (!increment(result.members))
                    return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
            } else if (!node()) return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
        } else {
            if (!node()) return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
            if (c == '{' || c == '[') {
                if (containers == parents.size())
                    return std::unexpected{WireJsonError{WireJsonFailure::byte_limit, containers + 1U}};
                parents[containers++] = c;
                ++i;
            } else {
                const auto begin = i;
                while (i < text.size() && !delimiter(text[i]) && text[i] != '"') ++i;
                if (begin == i) return std::unexpected{WireJsonError{WireJsonFailure::malformed}};
            }
        }
        if (result.depth > depth_limit)
            return std::unexpected{WireJsonError{WireJsonFailure::byte_limit, result.depth}};
    }
    if (containers || result.nodes == 0)
        return std::unexpected{WireJsonError{WireJsonFailure::malformed}};
    return result;
}

auto component_dom_stock(std::size_t nodes, std::size_t strings) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    using Json = nlohmann::json;
    constexpr auto element = sizeof(Json) + sizeof(Json::object_t::value_type) +
        sizeof(Json::object_t) + sizeof(Json::array_t) + sizeof(Json::string_t);
    const auto multiplied = multiply_component_stock(nodes, element);
    if (!multiplied) return std::unexpected{multiplied.error()};
    return add_wire_json_count(*multiplied, strings);
}
auto component_parser_stock(std::size_t bytes, std::size_t depth) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    const auto frames = multiply_component_stock(depth,
        wire_json_parser_frame_bytes() + sizeof(JsonRetirementFrame) + 1U);
    const auto lexical = multiply_component_stock(bytes, 129U);
    if (!frames) return std::unexpected{frames.error()};
    if (!lexical) return std::unexpected{lexical.error()};
    const auto with_lexer = add_wire_json_count(*lexical, 16384U);
    if (!with_lexer) return std::unexpected{with_lexer.error()};
    return add_wire_json_count(*frames, *with_lexer);
}
auto component_renderer_stock(std::size_t depth, std::size_t members, std::size_t bytes) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    const auto frames = multiply_component_stock(depth, sizeof(WireJsonRenderFrame));
    const auto references = multiply_component_stock(members, sizeof(WireJsonMemberReference));
    if (!frames) return std::unexpected{frames.error()};
    if (!references) return std::unexpected{references.error()};
    const auto workspace = add_wire_json_count(*frames, *references);
    if (!workspace) return std::unexpected{workspace.error()};
    return add_wire_json_count(*workspace, bytes);
}
} // namespace epycbench::runtime::wire_v1::detail
