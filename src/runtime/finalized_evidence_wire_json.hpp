#pragma once

#include <cstddef>
#include <expected>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::runtime::wire_v1::detail {
struct JsonRetirementFrame final {
    nlohmann::json* value{};
    nlohmann::json::iterator current{};
};
void retire_wire_json(nlohmann::json& value, std::vector<JsonRetirementFrame>& workspace) noexcept;
struct WireJson final {
    nlohmann::json value;
    std::vector<JsonRetirementFrame> retirement;
    explicit WireJson(std::size_t depth);
    WireJson(WireJson&&) noexcept;
    auto operator=(WireJson&&) noexcept -> WireJson&;
    ~WireJson();
    WireJson(const WireJson&) = delete;
    auto operator=(const WireJson&) -> WireJson& = delete;
};
enum class WireJsonFailure { malformed, duplicate, unsupported, overflow, byte_limit };
struct WireJsonError final {
    WireJsonFailure code;
    std::size_t observed{};
};
// Shared by byte/member counting and bounded overflow tests; never forms a huge view.
constexpr auto add_wire_json_count(std::size_t current, std::size_t additional) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    if (additional > std::numeric_limits<std::size_t>::max() - current)
        return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
    return current + additional;
}
// Exact logical workspace element types, shared with admission accounting/tests.
struct WireJsonMemberReference final {
    const std::string* key{};
    const nlohmann::json* value{};
};
struct WireJsonRenderFrame final {
    const nlohmann::json* value{};
    std::size_t next{}, begin{};
    bool entered{};
    nlohmann::json::const_iterator current{};
};
auto count_wire_json_string(std::string_view text) noexcept
    -> std::expected<std::size_t, WireJsonError>;
// Caller has already bounded value-node depth; traversal allocates nothing.
auto count_wire_json_value(const nlohmann::json& value) noexcept
    -> std::expected<std::size_t, WireJsonError>;
auto wire_json_parser_frame_bytes() noexcept -> std::size_t;
auto parse_wire_json(std::string_view text, std::size_t depth)
    -> std::expected<WireJson, WireJsonError>;
auto render_wire_json(const nlohmann::json& value, std::size_t depth, std::size_t node_count,
                      std::size_t byte_limit) -> std::expected<std::string, WireJsonError>;
} // namespace epycbench::runtime::wire_v1::detail
