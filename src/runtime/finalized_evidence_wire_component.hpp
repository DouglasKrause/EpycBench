#pragma once

#include "core/json_ownership.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/monitoring/monitor_source.hpp"
#include "runtime/finalized_evidence_wire_json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace epycbench::runtime::wire_v1::detail {
// Lexical reservation only, never schema or canonical admission. Every valid
// parsed value/key is conservatively counted before entering the real decoder.
struct ComponentWireShape final {
    std::size_t bytes{}, nodes{}, members{}, strings{}, elements{}, depth{};
};
[[nodiscard]] auto count_component_wire_shape(std::string_view, std::size_t depth_limit) noexcept
    -> std::expected<ComponentWireShape, WireJsonError>;
constexpr auto multiply_component_stock(std::size_t count, std::size_t size) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
        return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
    return count * size;
}
[[nodiscard]] auto component_dom_stock(std::size_t nodes, std::size_t strings) noexcept
    -> std::expected<std::size_t, WireJsonError>;
[[nodiscard]] auto component_parser_stock(std::size_t bytes, std::size_t depth) noexcept
    -> std::expected<std::size_t, WireJsonError>;
[[nodiscard]] auto component_renderer_stock(std::size_t depth, std::size_t members, std::size_t bytes) noexcept
    -> std::expected<std::size_t, WireJsonError>;

// Bounded classifiers require a non-failing sink. Record overflow defensively;
// returning false would terminate inside the authoritative kernel.
template <class Count> struct ClassifierWorkerTuple final {
    std::uint32_t worker{};
    monitoring::MonitoringSignalState state{};
    std::optional<Count> first, second;
};
template <class Count> struct ClassifierWorkerSink final {
    std::vector<ClassifierWorkerTuple<Count>> values;
    std::size_t count{};
    bool overflow{};
    explicit ClassifierWorkerSink(std::size_t capacity) : values(capacity) {}
    static auto append(void* context, core::WorkerId worker,
                       monitoring::MonitoringSignalState state,
                       std::optional<Count> first, std::optional<Count> second) noexcept -> bool {
        auto& self = *static_cast<ClassifierWorkerSink*>(context);
        if (self.count == self.values.size()) self.overflow = true;
        else self.values[self.count++] = {worker.value(), state, first, second};
        return true;
    }
};

// The projection visitor runs with either this nonowning counter or an owned DOM.
// All quantities are logical stock, not allocator capacities or heap measurements.
struct ComponentProjectionCount final {
    std::size_t nodes{1}, members{}, strings{}, elements{}, depth{1}, bytes{4}, children{};
    bool overflow{}, invalid{};
    ComponentProjectionCount() = default;
    ComponentProjectionCount(std::nullptr_t) noexcept {}
    ComponentProjectionCount(bool value) noexcept : bytes(value ? 4U : 5U) {}
    ComponentProjectionCount(std::string_view value) noexcept : strings(value.size()) {
        auto count = count_wire_json_string(value);
        if (count) bytes = *count;
        else { overflow = count.error().code == WireJsonFailure::overflow; invalid = !overflow; }
    }
    ComponentProjectionCount(const char* value) noexcept
        : ComponentProjectionCount(std::string_view{value}) {}
    ComponentProjectionCount(const std::string& value) noexcept
        : ComponentProjectionCount(std::string_view{value}) {}
    static auto object() noexcept -> ComponentProjectionCount {
        ComponentProjectionCount value; value.bytes = 2; return value;
    }
    static auto array() noexcept -> ComponentProjectionCount { return object(); }
    void add(std::size_t& target, std::size_t value) noexcept {
        const auto sum = add_wire_json_count(target, value);
        if (sum) target = *sum;
        else overflow = true;
    }
    void child(const ComponentProjectionCount& value) noexcept {
        overflow = overflow || value.overflow;
        invalid = invalid || value.invalid;
        add(nodes, value.nodes); add(members, value.members); add(strings, value.strings);
        add(elements, value.elements); add(bytes, value.bytes);
        const auto nested = add_wire_json_count(value.depth, 1U);
        if (nested) depth = std::max(depth, *nested); else overflow = true;
        if (children != 0) add(bytes, 1U);
        add(children, 1U);
    }
};
struct ProjectionDecimal final {
    std::array<char, 32> text{};
    std::size_t length{};
    template <std::integral T> explicit ProjectionDecimal(T value) noexcept {
        const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
        length = static_cast<std::size_t>(result.ptr - text.data());
    }
    auto view() const noexcept -> std::string_view { return {text.data(), length}; }
    operator ComponentProjectionCount() const noexcept { return ComponentProjectionCount{view()}; }
};
inline void to_json(nlohmann::json& target, const ProjectionDecimal& value) {
    target = value.view();
}
template <std::integral T> auto projection_decimal(T value) noexcept -> ProjectionDecimal {
    return ProjectionDecimal{value};
}
struct ProjectionBinary64 final {
    std::array<char, 16> text{};
    explicit ProjectionBinary64(double value) noexcept {
        constexpr char digits[] = "0123456789abcdef";
        auto bits = std::bit_cast<std::uint64_t>(value);
        for (std::size_t i = text.size(); i != 0U; --i) {
            text[i - 1U] = digits[bits & 0xfU]; bits >>= 4U;
        }
    }
    auto view() const noexcept -> std::string_view { return {text.data(), text.size()}; }
    operator ComponentProjectionCount() const noexcept { return ComponentProjectionCount{view()}; }
};
inline void to_json(nlohmann::json& target, const ProjectionBinary64& value) { target = value.view(); }
inline auto projection_binary64(double value) noexcept -> ProjectionBinary64 { return ProjectionBinary64{value}; }

namespace component_projection {
template <class Json> struct Cleanup {
    Json& value;
    ~Cleanup() {
        if constexpr (!std::same_as<Json, ComponentProjectionCount>)
            core::detail::retire_json(value);
    }
};
template <class Json, class Fill> auto object(Fill&& fill) -> Json {
    auto value = Json::object();
    Cleanup cleanup{value};
    fill(value);
    return Json(std::move(value));
}
template <class Json> void set(Json& parent, std::string_view key,
                               std::type_identity_t<Json>&& value) {
    if constexpr (std::same_as<Json, ComponentProjectionCount>) {
        parent.child(value);
        parent.add(parent.members, 1U);
        parent.add(parent.strings, key.size());
        const auto encoded = count_wire_json_string(key);
        if (encoded) parent.add(parent.bytes, *encoded);
        else { parent.overflow |= encoded.error().code == WireJsonFailure::overflow;
               parent.invalid |= encoded.error().code != WireJsonFailure::overflow; }
        parent.add(parent.bytes, 1U);
    } else core::detail::set_json(parent, key, std::move(value));
}
template <class Json> void append(Json& parent, std::type_identity_t<Json>&& value) {
    if constexpr (std::same_as<Json, ComponentProjectionCount>) {
        parent.child(value); parent.add(parent.elements, 1U);
    } else core::detail::append_json(parent, std::move(value));
}
} // namespace component_projection
} // namespace epycbench::runtime::wire_v1::detail
