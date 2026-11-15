#include "runtime/finalized_evidence_wire_json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <utility>

namespace epycbench::runtime::wire_v1::detail {
void retire_wire_json(nlohmann::json& value, std::vector<JsonRetirementFrame>& workspace) noexcept {
    if (!value.is_structured())
        return;
    std::size_t depth = 1;
    workspace[0] = {&value, value.begin()};
    while (depth != 0) {
        auto& frame = workspace[depth - 1];
        if (frame.current == frame.value->end()) {
            frame.value->clear();
            --depth;
        } else {
            auto& child = *frame.current++;
            if (child.is_structured()) {
                workspace[depth++] = {&child, child.begin()};
            }
        }
    }
}
WireJson::WireJson(std::size_t depth) : retirement(depth) {}
WireJson::WireJson(WireJson&& other) noexcept
    : value(std::move(other.value)), retirement(std::move(other.retirement)) {}
auto WireJson::operator=(WireJson&& other) noexcept -> WireJson& {
    if (this != &other) {
        retire_wire_json(value, retirement);
        value = std::move(other.value);
        retirement = std::move(other.retirement);
    }
    return *this;
}
WireJson::~WireJson() { retire_wire_json(value, retirement); }
namespace {
using Json = nlohmann::json;
struct Builder final : nlohmann::json_sax<Json> {
    struct Frame {
        Json* value{};
        std::string key;
    };
    WireJson root;
    std::vector<Frame> frames;
    std::size_t depth{};
    bool duplicate{};
    explicit Builder(std::size_t maximum) : root(maximum), frames(maximum) {}
    auto destination() -> Json& {
        if (depth == 0)
            return root.value;
        auto& frame = frames[depth - 1];
        if (frame.value->is_array()) {
            frame.value->push_back(nullptr);
            return frame.value->back();
        }
        return (*frame.value)[frame.key];
    }
    template <class T> auto scalar(T&& value) -> bool {
        destination() = std::forward<T>(value);
        return true;
    }
    auto null() -> bool override { return scalar(nullptr); }
    auto boolean(bool value) -> bool override { return scalar(value); }
    auto number_integer(number_integer_t value) -> bool override { return scalar(value); }
    auto number_unsigned(number_unsigned_t value) -> bool override { return scalar(value); }
    auto number_float(number_float_t, const string_t&) -> bool override { return false; }
    auto string(string_t& value) -> bool override { return scalar(std::move(value)); }
    auto binary(binary_t&) -> bool override { return false; }
    auto begin(bool object) -> bool {
        if (depth == frames.size())
            return false;
        auto& target = destination();
        target = object ? Json::object() : Json::array();
        frames[depth++].value = &target;
        return true;
    }
    auto start_object(std::size_t) -> bool override { return begin(true); }
    auto start_array(std::size_t) -> bool override { return begin(false); }
    auto key(string_t& key_value) -> bool override {
        auto& frame = frames[depth - 1];
        if (frame.value->contains(key_value)) {
            duplicate = true;
            return false;
        }
        frame.key = std::move(key_value);
        return true;
    }
    auto end_object() -> bool override {
        --depth;
        return true;
    }
    auto end_array() -> bool override {
        --depth;
        return true;
    }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&)
        -> bool override {
        return false;
    }
};
// Streaming Unicode decoding: no key-sized decoded scalar or UTF16 allocation.
struct Cursor {
    std::string_view text;
    std::size_t at{};
    std::uint16_t pending{};
    bool invalid{};
    auto scalar() -> std::uint32_t {
        const auto first = static_cast<unsigned char>(text[at++]);
        if (first < 0x80)
            return first;
        unsigned count{};
        std::uint32_t value{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            value = first & 0x1fU;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            value = first & 0xfU;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            value = first & 7U;
            minimum = 0x10000;
        } else {
            invalid = true;
            return 0;
        }
        if (count > text.size() - at) {
            invalid = true;
            return 0;
        }
        for (unsigned index = 0; index < count; ++index) {
            const auto next = static_cast<unsigned char>(text[at++]);
            if ((next & 0xc0U) != 0x80U) {
                invalid = true;
                return 0;
            }
            value = (value << 6U) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            invalid = true;
        return value;
    }
    auto unit() -> std::uint16_t {
        if (pending != 0)
            return std::exchange(pending, 0);
        auto value = scalar();
        if (value <= 0xffff)
            return static_cast<std::uint16_t>(value);
        value -= 0x10000;
        pending = static_cast<std::uint16_t>(0xdc00U + (value & 0x3ffU));
        return static_cast<std::uint16_t>(0xd800U + (value >> 10U));
    }
    auto done() const -> bool { return at == text.size() && pending == 0; }
};
auto less_utf16(std::string_view left, std::string_view right) -> bool {
    Cursor a{left}, b{right};
    while (!a.done() && !b.done()) {
        const auto x = a.unit(), y = b.unit();
        if (x != y)
            return x < y;
    }
    return a.done() && !b.done();
}
struct Sink {
    std::string* output{};
    std::size_t size{};
    bool overflow{};
    void append(std::string_view value) {
        const auto total = add_wire_json_count(size, value.size());
        if (!total) {
            overflow = true;
            return;
        }
        if (output)
            std::copy(value.begin(), value.end(),
                      output->begin() + static_cast<std::ptrdiff_t>(size));
        size = *total;
    }
    void character(char value) { append({&value, 1}); }
};
auto quoted(std::string_view text, Sink& sink) -> bool {
    sink.character('"');
    Cursor cursor{text};
    constexpr std::string_view hex = "0123456789abcdef";
    while (!cursor.done()) {
        const auto start = cursor.at;
        const auto scalar = cursor.scalar();
        if (cursor.invalid)
            return false;
        switch (scalar) {
        case '"':
            sink.append("\\\"");
            break;
        case '\\':
            sink.append("\\\\");
            break;
        case '\b':
            sink.append("\\b");
            break;
        case '\t':
            sink.append("\\t");
            break;
        case '\n':
            sink.append("\\n");
            break;
        case '\f':
            sink.append("\\f");
            break;
        case '\r':
            sink.append("\\r");
            break;
        default:
            if (scalar < 0x20) {
                sink.append("\\u00");
                sink.character(hex[scalar >> 4U]);
                sink.character(hex[scalar & 15U]);
            } else
                sink.append(text.substr(start, cursor.at - start));
        }
    }
    sink.character('"');
    return true;
}
using Member = WireJsonMemberReference;
using Frame = WireJsonRenderFrame;
auto walk(const Json& root, std::vector<Frame>& frames, std::vector<Member>& members, Sink& sink,
          bool rendering, std::size_t& member_count) -> bool {
    if (frames.empty())
        return false;
    std::size_t depth = 1, used{};
    frames[0] = {&root, 0, 0, false};
    while (depth != 0) {
        auto& frame = frames[depth - 1];
        const auto& value = *frame.value;
        if (!frame.entered) {
            frame.entered = true;
            if (value.is_object()) {
                frame.begin = used;
                if (rendering) {
                    if (value.size() > members.size() - used)
                        return false;
                    for (auto it = value.begin(); it != value.end(); ++it)
                        members[used++] = {&it.key(), &it.value()};
                    std::sort(members.begin() + static_cast<std::ptrdiff_t>(frame.begin),
                              members.begin() + static_cast<std::ptrdiff_t>(used),
                              [](const Member& a, const Member& b) {
                                  return less_utf16(*a.key, *b.key);
                              });
                } else {
                    const auto total = add_wire_json_count(member_count, value.size());
                    if (!total) {
                        sink.overflow = true;
                        return false;
                    }
                    member_count = *total;
                    frame.current = value.cbegin();
                }
                sink.character('{');
            } else if (value.is_array())
                sink.character('[');
            else {
                if (value.is_null())
                    sink.append("null");
                else if (value.is_boolean())
                    sink.append(value.get<bool>() ? "true" : "false");
                else if (value.is_number_integer()) {
                    std::array<char, 24> buffer{};
                    const auto converted =
                        value.is_number_unsigned()
                            ? std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                            value.get<std::uint64_t>())
                            : std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                            value.get<std::int64_t>());
                    sink.append(
                        {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())});
                } else if (value.is_string()) {
                    if (!quoted(value.get_ref<const std::string&>(), sink))
                        return false;
                } else
                    return false;
                --depth;
                continue;
            }
        }
        if (frame.next == value.size()) {
            sink.character(value.is_object() ? '}' : ']');
            if (value.is_object())
                used = frame.begin;
            --depth;
            continue;
        }
        if (frame.next != 0)
            sink.character(',');
        const Json* child{};
        if (value.is_object()) {
            if (rendering) {
                const auto& member = members[frame.begin + frame.next];
                if (!quoted(*member.key, sink))
                    return false;
                child = member.value;
            } else {
                if (!quoted(frame.current.key(), sink))
                    return false;
                child = &*frame.current++;
            }
            sink.character(':');
        } else
            child = &value[frame.next];
        ++frame.next;
        if (depth == frames.size())
            return false;
        frames[depth++] = {child, 0, 0, false};
    }
    return !sink.overflow;
}
} // namespace
auto count_wire_json_string(std::string_view text) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    Sink sink;
    if (!quoted(text, sink)) return std::unexpected{WireJsonError{WireJsonFailure::unsupported}};
    if (sink.overflow) return std::unexpected{WireJsonError{WireJsonFailure::overflow}};
    return sink.size;
}
auto count_wire_json_value(const Json& value) noexcept
    -> std::expected<std::size_t, WireJsonError> {
    if (value.is_string()) return count_wire_json_string(value.get_ref<const std::string&>());
    if (value.is_null()) return 4U;
    if (value.is_boolean()) return value.get<bool>() ? 4U : 5U;
    if (value.is_number_unsigned() || value.is_number_integer()) {
        char buffer[32];
        const auto converted = value.is_number_unsigned()
            ? std::to_chars(buffer, buffer + sizeof(buffer), value.get<std::uint64_t>())
            : std::to_chars(buffer, buffer + sizeof(buffer), value.get<std::int64_t>());
        return static_cast<std::size_t>(converted.ptr - buffer);
    }
    if (!value.is_structured()) return std::unexpected{WireJsonError{WireJsonFailure::unsupported}};
    std::size_t count = 2;
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
        auto item = count_wire_json_value(it.value());
        if (!item) return std::unexpected{item.error()};
        auto total = add_wire_json_count(count, *item);
        if (!total) return total;
        count = *total;
        if (value.is_object()) {
            auto key = count_wire_json_string(it.key());
            if (!key) return key;
            total = add_wire_json_count(count, *key);
            if (!total) return total;
            count = *total;
            total = add_wire_json_count(count, 1U);
            if (!total) return total;
            count = *total;
        }
        if (!first) {
            total = add_wire_json_count(count, 1U);
            if (!total) return total;
            count = *total;
        }
        first = false;
    }
    return count;
}
auto wire_json_parser_frame_bytes() noexcept -> std::size_t { return sizeof(Builder::Frame); }
auto parse_wire_json(std::string_view text, std::size_t depth)
    -> std::expected<WireJson, WireJsonError> {
    Builder builder{depth};
    if (!Json::sax_parse(text, &builder))
        return std::unexpected{WireJsonError{builder.duplicate ? WireJsonFailure::duplicate
                                                               : WireJsonFailure::malformed}};
    return std::move(builder.root);
}
auto render_wire_json(const Json& value, std::size_t depth, std::size_t node_count,
                      std::size_t byte_limit) -> std::expected<std::string, WireJsonError> {
    std::vector<Frame> frames(std::max(depth, std::size_t{1}));
    std::vector<Member> members;
    std::size_t member_count{};
    Sink count;
    if (!walk(value, frames, members, count, false, member_count))
        return std::unexpected{WireJsonError{count.overflow ? WireJsonFailure::overflow
                                                            : WireJsonFailure::unsupported}};
    if (count.size > byte_limit)
        return std::unexpected{WireJsonError{WireJsonFailure::byte_limit, count.size}};
    if (member_count > node_count)
        return std::unexpected{WireJsonError{WireJsonFailure::unsupported}};
    members.resize(member_count);
    std::string output(count.size, '\0');
    Sink render{&output};
    if (!walk(value, frames, members, render, true, member_count))
        return std::unexpected{WireJsonError{WireJsonFailure::unsupported}};
    return output;
}
} // namespace epycbench::runtime::wire_v1::detail
