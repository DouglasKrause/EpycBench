#include "topology/topology_json_parser.hpp"
#include "core/json_ownership.hpp"
#include <array>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace epycbench::topology::detail {
namespace {

auto parser_error(std::string code, std::string classification) -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        core::detail::set_json(object, "classification", std::move(classification));
    });
    core::detail::JsonCleanup cleanup{context};
    return {.category = core::ErrorCategory::topology,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Topology JSON input is invalid",
            .operation = "parse_topology_json",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto check_limits(const nlohmann::json& value, std::size_t depth)
    -> std::expected<void, core::Error> {
    if (depth > topology_json_max_depth)
        return std::unexpected{parser_error("topology.serialization.limit_exceeded", "depth")};
    if (value.is_string() &&
        value.get_ref<const std::string&>().size() > topology_json_max_string_bytes)
        return std::unexpected{
            parser_error("topology.serialization.limit_exceeded", "string_bytes")};
    if ((value.is_array() || value.is_object()) && value.size() > topology_json_max_array_entries)
        return std::unexpected{
            parser_error("topology.serialization.limit_exceeded", "container_entries")};
    if (value.is_array() || value.is_object()) {
        for (const auto& child : value)
            if (auto checked = check_limits(child, depth + 1U); !checked)
                return decltype(checked)(std::move(checked));
    }
    return {};
}

// SAX owns no JSON temporaries. Continue lexical parsing after duplicate/depth
// rejection so the legacy duplicate > depth > syntax precedence is unchanged.
struct Builder : nlohmann::json_sax<nlohmann::json> {
    using Json = nlohmann::json;
    struct Frame {
        Json* value{};
        std::set<std::string, std::less<>> keys;
        std::string key;
    };
    Json root;
    core::detail::JsonCleanup<Json> cleanup{root};
    std::array<Frame, topology_json_max_depth + 1> frames;
    std::size_t depth{};
    bool duplicate{}, depth_exceeded{};
    auto allowed(std::size_t event_depth) -> bool {
        if (event_depth > topology_json_max_depth)
            depth_exceeded = true;
        return !duplicate && !depth_exceeded;
    }
    auto destination() -> Json& {
        if (depth == 0)
            return root;
        auto& frame = frames[depth - 1];
        if (frame.value->is_array()) {
            frame.value->push_back(nullptr);
            return frame.value->back();
        }
        return (*frame.value)[frame.key];
    }
    template <class T> auto scalar(T&& value) -> bool {
        if (allowed(depth))
            destination() = std::forward<T>(value);
        return true;
    }
    auto null() -> bool override { return scalar(nullptr); }
    auto boolean(bool value) -> bool override { return scalar(value); }
    auto number_integer(number_integer_t value) -> bool override { return scalar(value); }
    auto number_unsigned(number_unsigned_t value) -> bool override { return scalar(value); }
    auto number_float(number_float_t value, const string_t&) -> bool override {
        return scalar(value);
    }
    auto string(string_t& value) -> bool override { return scalar(std::move(value)); }
    auto binary(binary_t&) -> bool override { return false; }
    auto begin(bool object) -> bool {
        const bool build = allowed(depth);
        if (depth <= topology_json_max_depth) {
            auto& frame = frames[depth];
            frame.keys.clear();
            frame.key.clear();
            frame.value = nullptr;
            if (build) {
                auto& value = destination();
                value = object ? Json::object() : Json::array();
                frame.value = &value;
            }
        }
        ++depth;
        return true;
    }
    auto start_object(std::size_t) -> bool override { return begin(true); }
    auto start_array(std::size_t) -> bool override { return begin(false); }
    auto key(string_t& value) -> bool override {
        // Legacy callbacks evaluate depth before processing a key. Keep detecting
        // shallow duplicates even after an earlier depth failure.
        if (depth > topology_json_max_depth) {
            depth_exceeded = true;
            return true;
        }
        auto& frame = frames[depth - 1];
        if (!frame.keys.insert(value).second)
            duplicate = true;
        if (!duplicate && !depth_exceeded)
            frame.key = std::move(value);
        return true;
    }
    auto end_object() -> bool override {
        --depth;
        allowed(depth);
        return true;
    }
    auto end_array() -> bool override {
        --depth;
        allowed(depth);
        return true;
    }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&)
        -> bool override {
        return false;
    }
};

} // namespace

auto parse_topology_json(std::string_view input) -> std::expected<nlohmann::json, core::Error> {
    if (input.size() > topology_json_max_document_bytes)
        return std::unexpected{
            parser_error("topology.serialization.limit_exceeded", "document_bytes")};

    Builder builder;
    const bool parsed = nlohmann::json::sax_parse(input, &builder,
                                                  nlohmann::json::input_format_t::json, true, true);
    if (builder.duplicate)
        return std::unexpected{
            parser_error("topology.serialization.duplicate_member", "duplicate_member")};
    if (builder.depth_exceeded)
        return std::unexpected{parser_error("topology.serialization.limit_exceeded", "depth")};
    if (!parsed)
        return std::unexpected{parser_error("topology.serialization.parse_failed", "syntax")};
    if (auto checked = check_limits(builder.root, 0U); !checked)
        return std::unexpected{std::move(checked.error())};
    return nlohmann::json(std::move(builder.root));
}

} // namespace epycbench::topology::detail
