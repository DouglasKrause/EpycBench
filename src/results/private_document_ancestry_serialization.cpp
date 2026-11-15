#include "results/private_document_ancestry_serialization.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace epycbench::results::document_ancestry_serialization {
namespace detail {
struct State {
    std::array<core::detail::JsonOwner, 3> trees{Json{}, Json{}, Json{}};
    std::string name;
    LocationMap locations;
    OriginMap origins;
    std::string canonical;
    core::Sha256Digest digest{core::sha256(std::string_view{})};
    Origin origin{};
    Usage usage;
};
bool checked_add(std::size_t& value, std::size_t amount) noexcept {
    if (amount > SIZE_MAX - value) return false;
    value += amount;
    return true;
}
auto checked_multiply(std::size_t a, std::size_t b) noexcept -> std::optional<std::size_t> {
    if (b && a > SIZE_MAX / b) return {};
    return a * b;
}
}
namespace {
static_assert(std::is_same_v<Json::number_integer_t, std::int64_t>);
static_assert(std::is_same_v<Json::number_unsigned_t, std::uint64_t>);
static_assert(std::is_same_v<Json::number_float_t, double>);
static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);
constexpr std::array<std::string_view, 6> origin_names{
    "explicit_value", "schema_default", "scenario_default", "experiment_override",
    "variant_override", "matrix_override"};
constexpr std::array<std::size_t Usage::*, 11> quantities{
    &Usage::tree_depth, &Usage::location_rows, &Usage::origin_rows, &Usage::source_name_bytes,
    &Usage::tree_octets, &Usage::metadata_octets, &Usage::index_entries,
    &Usage::pointer_scratch_bytes, &Usage::canonical_bytes, &Usage::retained_bytes,
    &Usage::consumer_simultaneous_bytes};
void require(bool condition, Error error = Error::schema) {
    if (!condition) throw error;
}
auto add(std::size_t a, std::size_t b) -> std::size_t {
    require(detail::checked_add(a, b), Error::capacity);
    return a;
}
auto mul(std::size_t a, std::size_t b) -> std::size_t {
    const auto value = detail::checked_multiply(a, b);
    require(value.has_value(), Error::capacity);
    return *value;
}
void bound(std::size_t value, std::size_t limit) { require(value <= limit, Error::capacity); }
void valid_limits(const Limits& limits) {
    require(limits.tree_depth <= 64, Error::invalid_limits);
}
struct Decimal {
    std::array<char, 21> bytes{};
    std::size_t size{};
    template<class T> explicit Decimal(T value) {
        const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
        require(result.ec == std::errc{}, Error::capacity);
        size = static_cast<std::size_t>(result.ptr - bytes.data());
    }
    auto view() const noexcept -> std::string_view { return {bytes.data(), size}; }
};
auto nibble(char value) -> unsigned {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    require(value >= 'a' && value <= 'f', Error::encoding);
    return static_cast<unsigned>(value - 'a' + 10);
}
auto octet(std::string_view value, std::size_t at) -> unsigned {
    return (nibble(value[at]) << 4U) | nibble(value[at + 1]);
}
void valid_hex(std::string_view value) {
    require(value.size() % 2 == 0, Error::encoding);
    for (const auto ch : value) (void)nibble(ch);
}
auto unhex(std::string_view value) -> std::string {
    std::string result(value.size() / 2, '\0');
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(octet(value, i * 2));
    return result;
}
template<class T> auto number(std::string_view text) -> T {
    require(!text.empty(), Error::encoding);
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), Error::encoding);
    const Decimal canonical{value};
    require(canonical.view() == text, Error::encoding);
    return value;
}
auto bits(std::string_view value) -> std::uint64_t {
    require(value.size() == 16, Error::encoding);
    std::uint64_t result{};
    for (const auto ch : value) result = (result << 4U) | nibble(ch);
    require((result & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL, Error::schema);
    return result;
}
auto origin_name(scenario::ValueOriginKind kind) -> std::string_view {
    const auto index = static_cast<std::size_t>(kind);
    require(index < origin_names.size());
    return origin_names[index];
}
auto origin_kind(std::string_view name) -> scenario::ValueOriginKind {
    for (std::size_t i = 0; i < origin_names.size(); ++i)
        if (origin_names[i] == name) return static_cast<scenario::ValueOriginKind>(i);
    throw Error::schema;
}
struct Counts {
    Usage usage;
    std::size_t dynamic{}, maximum{}, width{}, path{}, objects{}, arrays{}, members{}, strings{};
};
void text_stock(Counts& count, std::size_t size, bool metadata) {
    bound(size, std::string{}.max_size());
    auto& target = metadata ? count.usage.metadata_octets : count.usage.tree_octets;
    target = add(target, size);
    count.maximum = std::max(count.maximum, size);
}
void node_stock(Counts& count, std::size_t tree, std::size_t depth, Json::value_t type,
                std::size_t size) {
    count.usage.tree_nodes[tree] = add(count.usage.tree_nodes[tree], 1);
    count.usage.tree_depth = std::max(count.usage.tree_depth, depth);
    if (depth != 0) count.dynamic = add(count.dynamic, sizeof(Json));
    if (type == Json::value_t::object) {
        bound(size, Json::object_t::Container{}.max_size());
        ++count.objects;
        count.members = add(count.members, size);
        count.width = std::max(count.width, size);
        count.dynamic = add(count.dynamic, add(sizeof(Json::object_t), mul(size, sizeof(Json::string_t))));
    } else if (type == Json::value_t::array) {
        bound(size, Json::array_t{}.max_size());
        ++count.arrays;
        count.dynamic = add(count.dynamic, sizeof(Json::array_t));
    } else if (type == Json::value_t::string) {
        ++count.strings;
        count.dynamic = add(count.dynamic, sizeof(Json::string_t));
    }
}
auto escaped_length(std::string_view value) -> std::size_t {
    std::size_t result{};
    for (const auto ch : value) result = add(result, ch == '~' || ch == '/' ? 2 : 1);
    return result;
}
void measure_tree(const Json& value, Counts& count, const Limits& limits, std::size_t tree,
                  std::size_t depth, std::size_t path) {
    bound(depth, limits.tree_depth);
    const auto type = value.type();
    require(type != Json::value_t::binary && type != Json::value_t::discarded);
    if (value.is_number_float())
        require((std::bit_cast<std::uint64_t>(value.get_ref<const double&>()) &
                 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL);
    node_stock(count, tree, depth, type, value.is_structured() ? value.size() : 0);
    bound(count.usage.tree_nodes[tree], limits.tree_nodes[tree]);
    count.path = std::max(count.path, path);
    if (value.is_string()) {
        text_stock(count, value.get_ref<const std::string&>().size(), false);
        bound(count.usage.tree_octets, limits.tree_octets);
    }
    if (value.is_structured()) {
        std::size_t index{};
        for (auto it = value.cbegin(); it != value.cend(); ++it, ++index) {
            std::size_t component{};
            if (value.is_object()) {
                text_stock(count, it.key().size(), false);
                bound(count.usage.tree_octets, limits.tree_octets);
                component = escaped_length(it.key());
            } else component = Decimal{index}.size;
            measure_tree(it.value(), count, limits, tree, depth + 1, add(add(path, 1), component));
        }
    }
    bound(count.usage.tree_octets, limits.tree_octets);
}
struct Sink {
    std::string* output{};
    std::size_t size{};
    std::size_t ceiling{SIZE_MAX};
    void raw(std::string_view value) {
        size = add(size, value.size());
        bound(size, ceiling);
        if (output) output->append(value);
    }
    void character(char value) { raw({&value, 1}); }
    void string(std::string_view value) { character('"'); raw(value); character('"'); }
    template<class T> void integer(T value) { const Decimal decimal{value}; string(decimal.view()); }
    void hex(std::string_view value) {
        bound(add(size, add(mul(value.size(), 2), 2)), ceiling);
        character('"');
        for (const auto raw_value : value) {
            const auto byte = static_cast<unsigned char>(raw_value);
            character("0123456789abcdef"[byte >> 4U]);
            character("0123456789abcdef"[byte & 15U]);
        }
        character('"');
    }
    void floating(double value) {
        const auto binary = std::bit_cast<std::uint64_t>(value);
        std::array<char, 16> encoded{};
        for (std::size_t i = 0; i < encoded.size(); ++i)
            encoded[i] = "0123456789abcdef"[(binary >> ((15 - i) * 4)) & 15U];
        string({encoded.data(), encoded.size()});
    }
};
void tree_rows(Sink& sink, const Json& value) {
    sink.character('[');
    switch (value.type()) {
    case Json::value_t::null: sink.string("null"); break;
    case Json::value_t::boolean:
        sink.raw("\"boolean\","); sink.raw(value.get<bool>() ? "true" : "false"); break;
    case Json::value_t::number_integer:
        sink.raw("\"signed\","); sink.integer(value.get<std::int64_t>()); break;
    case Json::value_t::number_unsigned:
        sink.raw("\"unsigned\","); sink.integer(value.get<std::uint64_t>()); break;
    case Json::value_t::number_float:
        sink.raw("\"binary64\","); sink.floating(value.get<double>()); break;
    case Json::value_t::string:
        sink.raw("\"string\","); sink.hex(value.get_ref<const std::string&>()); break;
    case Json::value_t::array:
        sink.raw("\"array\","); sink.integer(value.size()); break;
    case Json::value_t::object: {
        sink.raw("\"object\",[");
        bool comma{};
        for (auto it = value.cbegin(); it != value.cend(); ++it) {
            if (comma) sink.character(',');
            comma = true;
            sink.hex(it.key());
        }
        sink.character(']');
        break;
    }
    default: throw Error::schema;
    }
    sink.character(']');
    if (value.is_structured()) for (const auto& child : value) {
        sink.character(',');
        tree_rows(sink, child);
    }
}
void location_row(Sink& sink, const scenario::SourceLocation& value) {
    sink.character('['); sink.integer(value.byte_offset); sink.character(',');
    sink.integer(value.line); sink.character(','); sink.integer(value.column); sink.character(']');
}
struct View {
    std::array<const Json*, 3> trees;
    std::string_view name;
    const LocationMap* locations;
    const OriginMap* origins;
};
void emit(Sink& sink, const View& view) {
    sink.raw("{\"authored\":["); tree_rows(sink, *view.trees[0]);
    sink.raw("],\"default_expanded\":["); tree_rows(sink, *view.trees[2]);
    sink.raw("],\"locations\":[");
    bool comma{};
    for (const auto& [key, value] : *view.locations) {
        if (comma) sink.character(',');
        comma = true;
        sink.character('['); sink.hex(key); sink.character(','); location_row(sink, value.value);
        sink.character(',');
        if (value.mapping_key) location_row(sink, *value.mapping_key); else sink.raw("null");
        sink.character(']');
    }
    sink.raw("],\"origins\":["); comma = false;
    for (const auto& [key, value] : *view.origins) {
        if (comma) sink.character(',');
        comma = true;
        sink.character('['); sink.hex(key); sink.character(','); sink.string(origin_name(value.kind));
        sink.character(',');
        if (value.source_pointer) sink.hex(*value.source_pointer); else sink.raw("null");
        sink.character(']');
    }
    sink.raw("],\"schema\":"); sink.string(profile);
    sink.raw(",\"source_name\":"); sink.hex(view.name);
    sink.raw(",\"unit_normalized\":["); tree_rows(sink, *view.trees[1]); sink.raw("]}");
}
auto view(const detail::State& state) -> View {
    return {{&state.trees[0].value, &state.trees[1].value, &state.trees[2].value},
            state.name, &state.locations, &state.origins};
}
auto measure(const View& source, const Limits& limits) -> Counts {
    Counts count;
    bound(source.locations->size(), limits.location_rows);
    bound(source.origins->size(), limits.origin_rows);
    bound(source.name.size(), limits.source_name_bytes);
    for (std::size_t i = 0; i < 3; ++i) measure_tree(*source.trees[i], count, limits, i, 0, 0);
    count.usage.source_name_bytes = source.name.size();
    count.maximum = std::max(count.maximum, source.name.size());
    count.usage.location_rows = source.locations->size();
    count.usage.origin_rows = source.origins->size();
    for (const auto& [key, value] : *source.locations) {
        (void)value;
        text_stock(count, key.size(), true);
        bound(count.usage.metadata_octets, limits.metadata_octets);
        count.path = std::max(count.path, key.size());
    }
    for (const auto& [key, value] : *source.origins) {
        text_stock(count, key.size(), true);
        bound(count.usage.metadata_octets, limits.metadata_octets);
        count.path = std::max(count.path, key.size());
        (void)origin_name(value.kind);
        if (value.source_pointer) {
            text_stock(count, value.source_pointer->size(), true);
            bound(count.usage.metadata_octets, limits.metadata_octets);
            count.path = std::max(count.path, value.source_pointer->size());
        }
    }
    Sink size{nullptr, 0, limits.canonical_bytes};
    emit(size, source);
    count.usage.canonical_bytes = size.size;
    return count;
}
struct Reader {
    std::string_view body;
    std::size_t at{};
    void literal(std::string_view value) {
        require(body.substr(at, value.size()) == value, Error::encoding);
        at += value.size();
    }
    bool next(char value) const noexcept { return at < body.size() && body[at] == value; }
    auto string() -> std::string_view {
        literal("\"");
        const auto start = at;
        while (at < body.size() && body[at] != '"') {
            require(static_cast<unsigned char>(body[at]) >= 0x20 && body[at] != '\\', Error::encoding);
            ++at;
        }
        require(at < body.size(), Error::encoding);
        const auto result = body.substr(start, at - start);
        ++at;
        return result;
    }
    auto hex() -> std::string_view { const auto result = string(); valid_hex(result); return result; }
};
struct BuildFrame {
    Json* node{};
    std::string_view keys;
    std::size_t remaining{}, key_at{}, index{}, path{};
    bool object{};
};
struct Parser {
    Reader reader;
    const Limits* limits;
    detail::State* output;
    std::vector<std::string_view>* key_index;
    Counts count;
    std::array<BuildFrame, 65> frames{};
    auto location() -> scenario::SourceLocation {
        reader.literal("[");
        const auto offset = number<std::uint64_t>(reader.string()); reader.literal(",");
        const auto line = number<std::uint64_t>(reader.string()); reader.literal(",");
        const auto column = number<std::uint64_t>(reader.string()); reader.literal("]");
        return {offset, line, column};
    }
    void tree(std::size_t which) {
        reader.literal("[");
        std::size_t active{};
        bool first = true;
        while (!reader.next(']')) {
            while (active && frames[active - 1].remaining == 0) --active;
            require(first || active != 0);
            if (!first) reader.literal(",");
            auto* parent = active ? &frames[active - 1] : nullptr;
            std::string_view key;
            std::size_t path{};
            if (parent) {
                --parent->remaining;
                std::size_t length{};
                if (parent->object) {
                    Reader keys{parent->keys, parent->key_at};
                    if (parent->index) keys.literal(",");
                    key = keys.hex();
                    parent->key_at = keys.at;
                    for (std::size_t i = 0; i < key.size(); i += 2) {
                        const auto byte = octet(key, i);
                        length = add(length, byte == '~' || byte == '/' ? 2 : 1);
                    }
                } else length = Decimal{parent->index}.size;
                ++parent->index;
                path = add(add(parent->path, 1), length);
            }
            count.path = std::max(count.path, path);
            bound(active, limits->tree_depth);
            reader.literal("[");
            const auto tag = reader.string();
            Json node;
            core::detail::JsonCleanup cleanup{node};
            Json::value_t type{};
            std::size_t children{};
            std::string_view keys;
            if (tag == "null") type = Json::value_t::null;
            else if (tag == "boolean") {
                type = Json::value_t::boolean;
                reader.literal(",");
                const bool value = reader.next('t');
                reader.literal(value ? "true" : "false");
                if (output) node = value;
            } else if (tag == "signed") {
                type = Json::value_t::number_integer;
                reader.literal(",");
                const auto value = number<std::int64_t>(reader.string());
                if (output) node = value;
            } else if (tag == "unsigned") {
                type = Json::value_t::number_unsigned;
                reader.literal(",");
                const auto value = number<std::uint64_t>(reader.string());
                if (output) node = value;
            } else if (tag == "binary64") {
                type = Json::value_t::number_float;
                reader.literal(",");
                const auto value = bits(reader.string());
                if (output) node = std::bit_cast<double>(value);
            } else if (tag == "string") {
                type = Json::value_t::string;
                reader.literal(",");
                const auto value = reader.hex();
                text_stock(count, value.size() / 2, false);
                if (output) node = unhex(value);
            } else if (tag == "array") {
                type = Json::value_t::array;
                reader.literal(",");
                children = number<std::size_t>(reader.string());
                if (output) {
                    node = Json::array();
                    node.get_ref<Json::array_t&>().reserve(children);
                }
            } else if (tag == "object") {
                type = Json::value_t::object;
                reader.literal(",[");
                const auto start = reader.at;
                if (key_index) key_index->clear();
                while (!reader.next(']')) {
                    if (children) reader.literal(",");
                    const auto name = reader.hex();
                    children = add(children, 1);
                    bound(children, limits->tree_nodes[which]);
                    text_stock(count, name.size() / 2, false);
                    if (key_index) key_index->push_back(name);
                }
                keys = reader.body.substr(start, reader.at - start);
                reader.literal("]");
                if (key_index) {
                    std::make_heap(key_index->begin(), key_index->end());
                    std::sort_heap(key_index->begin(), key_index->end());
                    for (std::size_t i = 1; i < key_index->size(); ++i)
                        require((*key_index)[i - 1] != (*key_index)[i]);
                }
                if (output) {
                    node = Json::object();
                    using Container = Json::object_t::Container;
                    static_cast<Container&>(node.get_ref<Json::object_t&>()).reserve(children);
                }
            } else throw Error::schema;
            reader.literal("]");
            node_stock(count, which, active, type, children);
            bound(count.usage.tree_nodes[which], limits->tree_nodes[which]);
            bound(children, limits->tree_nodes[which] - count.usage.tree_nodes[which]);
            bound(count.usage.tree_octets, limits->tree_octets);
            if (first) require(type == Json::value_t::object);
            Json* attached{};
            if (output) {
                if (!parent) {
                    output->trees[which].value = std::move(node);
                    attached = &output->trees[which].value;
                } else if (parent->object) {
                    const auto decoded = unhex(key);
                    core::detail::append_object(*parent->node, decoded, node);
                    attached = &parent->node->back();
                } else {
                    parent->node->push_back(std::move(node));
                    attached = &parent->node->back();
                }
            }
            if (children) {
                require(active < frames.size(), Error::capacity);
                frames[active++] = {attached, keys, children, 0, 0, path, type == Json::value_t::object};
            }
            first = false;
        }
        while (active && frames[active - 1].remaining == 0) --active;
        require(!first && active == 0);
        reader.literal("]");
    }
    auto metadata_key(std::string_view& previous, bool first) -> std::string_view {
        const auto key = reader.hex();
        require(first || previous < key);
        previous = key;
        text_stock(count, key.size() / 2, true);
        count.path = std::max(count.path, key.size() / 2);
        return key;
    }
    auto parse() -> Counts {
        bound(reader.body.size(), limits->canonical_bytes);
        reader.literal("{\"authored\":"); tree(0);
        reader.literal(",\"default_expanded\":"); tree(2);
        reader.literal(",\"locations\":[");
        std::string_view previous;
        while (!reader.next(']')) {
            if (count.usage.location_rows) reader.literal(",");
            reader.literal("[");
            const auto key = metadata_key(previous, count.usage.location_rows == 0);
            reader.literal(",");
            const auto value = location();
            reader.literal(",");
            std::optional<scenario::SourceLocation> mapping_key;
            if (reader.next('n')) reader.literal("null"); else mapping_key = location();
            reader.literal("]");
            count.usage.location_rows = add(count.usage.location_rows, 1);
            bound(count.usage.location_rows, limits->location_rows);
            bound(count.usage.metadata_octets, limits->metadata_octets);
            if (output) output->locations.emplace(unhex(key), scenario::NodeLocations{value, mapping_key});
        }
        reader.literal("],\"origins\":["); previous = {};
        while (!reader.next(']')) {
            if (count.usage.origin_rows) reader.literal(",");
            reader.literal("[");
            const auto key = metadata_key(previous, count.usage.origin_rows == 0);
            reader.literal(",");
            const auto kind = origin_kind(reader.string());
            reader.literal(",");
            std::optional<std::string_view> source;
            if (reader.next('n')) reader.literal("null");
            else {
                source = reader.hex();
                text_stock(count, source->size() / 2, true);
                count.path = std::max(count.path, source->size() / 2);
            }
            reader.literal("]");
            count.usage.origin_rows = add(count.usage.origin_rows, 1);
            bound(count.usage.origin_rows, limits->origin_rows);
            bound(count.usage.metadata_octets, limits->metadata_octets);
            if (output) {
                std::optional<std::string> decoded;
                if (source) decoded = unhex(*source);
                output->origins.emplace(unhex(key), scenario::ValueOrigin{kind, std::move(decoded)});
            }
        }
        reader.literal("],\"schema\":"); require(reader.string() == profile);
        reader.literal(",\"source_name\":");
        const auto name = reader.hex();
        count.usage.source_name_bytes = name.size() / 2;
        count.maximum = std::max(count.maximum, name.size() / 2);
        bound(count.usage.source_name_bytes, limits->source_name_bytes);
        if (output) output->name = unhex(name);
        reader.literal(",\"unit_normalized\":"); tree(1);
        reader.literal("}"); require(reader.at == reader.body.size(), Error::encoding);
        count.usage.canonical_bytes = reader.body.size();
        return count;
    }
};
void append_token(std::string& path, std::string_view token) {
    path.push_back('/');
    for (const auto ch : token) {
        if (ch == '~') path.append("~0");
        else if (ch == '/') path.append("~1");
        else path.push_back(ch);
    }
}
void check_locations(const Json& node, const LocationMap& locations, std::string& path,
                     bool object_member) {
    const auto found = locations.find(path);
    require(found != locations.end() && found->second.mapping_key.has_value() == object_member,
            Error::reference);
    if (!node.is_structured()) return;
    std::size_t index{};
    for (auto it = node.cbegin(); it != node.cend(); ++it, ++index) {
        const auto original = path.size();
        if (node.is_object()) append_token(path, it.key());
        else append_token(path, Decimal{index}.view());
        check_locations(it.value(), locations, path, node.is_object());
        path.resize(original);
    }
}
auto resolve_pointer(const Json& root, std::string_view pointer, std::string& token) -> const Json& {
    const Json* node = &root;
    std::size_t at{};
    while (at < pointer.size()) {
        require(pointer[at++] == '/', Error::reference);
        token.clear();
        while (at < pointer.size() && pointer[at] != '/') {
            const auto ch = pointer[at++];
            if (ch != '~') token.push_back(ch);
            else {
                require(at < pointer.size() && (pointer[at] == '0' || pointer[at] == '1'), Error::reference);
                token.push_back(pointer[at++] == '0' ? '~' : '/');
            }
        }
        if (node->is_object()) {
            const auto child = node->find(token);
            require(child != node->end(), Error::reference);
            node = &*child;
        } else if (node->is_array()) {
            const auto index = number<std::size_t>(token);
            require(index < node->size(), Error::reference);
            node = &(*node)[index];
        } else throw Error::reference;
    }
    return *node;
}
auto equal(const Json& left, const Json& right) -> bool {
    if (left.type() != right.type()) return false;
    if (left.is_number_float())
        return std::bit_cast<std::uint64_t>(left.get_ref<const double&>()) ==
               std::bit_cast<std::uint64_t>(right.get_ref<const double&>());
    if (!left.is_structured()) return left == right;
    if (left.size() != right.size()) return false;
    auto a = left.cbegin();
    auto b = right.cbegin();
    for (; a != left.cend(); ++a, ++b)
        if ((left.is_object() && a.key() != b.key()) || !equal(a.value(), b.value())) return false;
    return true;
}
void local(const View& source, const Counts& count) {
    for (const auto* tree : source.trees) {
        require(tree->is_object(), Error::state);
        const auto schema = tree->find("schema");
        require(schema != tree->end() && core::detail::json_string_equal(*schema, "epycbench.scenario/v1"), Error::state);
    }
    require(source.locations->size() == count.usage.tree_nodes[0], Error::reference);
    bound(source.origins->size(), count.usage.tree_nodes[2]);
    std::string path;
    std::string token;
    path.reserve(count.path);
    token.reserve(count.path);
    check_locations(*source.trees[0], *source.locations, path, false);
    for (const auto& [target, origin] : *source.origins) {
        const auto& value = resolve_pointer(*source.trees[2], target, token);
        if (origin.kind == scenario::ValueOriginKind::schema_default)
            require(!origin.source_pointer, Error::reference);
        else {
            require(origin.source_pointer.has_value(), Error::reference);
            if (origin.kind == scenario::ValueOriginKind::scenario_default)
                require(origin.source_pointer->starts_with("/defaults/"), Error::reference);
            else require(*origin.source_pointer == target, Error::reference);
            require(equal(value, resolve_pointer(*source.trees[1], *origin.source_pointer, token)), Error::reference);
        }
    }
}
// These are explicit conservative call-cell inventories, not native stack measurements.
// Recursive entries correspond to the actual bounded helpers above or json_ownership.hpp.
struct MeasureFrameCells {
    const Json* value;
    Counts* count;
    const Limits* limits;
    std::size_t tree, depth, path, index, component;
    Json::value_t type;
    Json::const_iterator it, end;
    const Json* child;
    const std::string* key;
    const std::string* scalar;
    const double* floating;
    std::uint64_t floating_bits;
    Decimal index_decimal;
};
struct EmitFrameCells {
    Sink* sink;
    const Json* value;
    Json::value_t type;
    bool comma, boolean;
    std::int64_t signed_value;
    std::uint64_t unsigned_value;
    double floating_value;
    const std::string* string_value;
    Json::const_iterator it, end, children_begin, children_end;
    const std::string* key;
    const Json* child;
};
struct CloneFrameCells {
    const Json* source;
    Json result, child, returned_scalar;
    core::detail::JsonCleanup<Json> result_guard, child_guard;
    Json::const_iterator it, end, array_begin, array_end;
    const Json* value;
    Json::object_t* object;
    Json::object_t::Container* container;
    Json::array_t* array;
};
struct RetireFrameCells {
    Json* value;
    Json::iterator begin, end;
    Json* child;
};
struct LocationFrameCells {
    const Json* node;
    const LocationMap* locations;
    std::string* path;
    bool object_member;
    LocationMap::const_iterator found, end;
    std::size_t index, original;
    Json::const_iterator it, node_end;
    const Json* value;
    const std::string* key;
    Decimal index_decimal;
};
struct EqualFrameCells {
    const Json* left;
    const Json* right;
    Json::const_iterator a, b, end;
    const Json* a_value;
    const Json* b_value;
    const std::string* a_key;
    const std::string* b_key;
    const double* left_float;
    const double* right_float;
    std::uint64_t left_bits, right_bits;
    bool result;
};
struct PrimitiveCells {
    bool require_condition;
    Error require_error;
    std::size_t add_a, add_b, mul_a, mul_b, bound_value, bound_limit;
    std::size_t* checked_value;
    std::size_t checked_amount, checked_a, checked_b;
    bool checked_result;
    std::optional<std::size_t> product, product_return;
    const Limits* valid_limits_argument;
    Decimal* decimal_this;
    std::int64_t decimal_signed;
    std::uint64_t decimal_unsigned;
    std::to_chars_result decimal_conversion;
    const Decimal* decimal_view_this;
    std::string_view decimal_view_return;
    char nibble_argument;
    unsigned nibble_return, octet_return;
    std::string_view octet_input, hex_input, unhex_input, number_input, bits_input, origin_input;
    std::size_t octet_at, unhex_index, origin_index;
    const char* hex_begin;
    const char* hex_end;
    char hex_value;
    std::string unhex_output, unhex_return;
    std::int64_t parsed_signed;
    std::uint64_t parsed_unsigned, parsed_bits;
    std::from_chars_result number_conversion;
    Decimal number_canonical;
    const char* bits_begin;
    const char* bits_end;
    char bit_character;
    scenario::ValueOriginKind origin_argument, origin_return;
    std::string_view origin_name_return;
};
struct ProbeCells {
    const scenario::ScenarioDigestedPlan* plan;
    std::string_view input;
    Limits limits;
    std::expected<Usage, Error> result;
    Error caught;
    std::unexpected<Error> error_return, capacity_return;
    const std::length_error* length_error;
};
struct CountCells {
    Counts* text_count;
    std::size_t text_size;
    bool text_metadata;
    std::size_t* text_target;
    Counts* node_count;
    std::size_t node_tree, node_depth, node_size;
    Json::value_t node_type;
    std::string_view escape_input;
    std::size_t escape_result;
    const char* escape_begin;
    const char* escape_end;
    char escape_character;
    const View* measure_source;
    const Limits* measure_limits;
    Counts measure_count, measure_return;
    std::size_t measure_index;
    LocationMap::const_iterator location_begin, location_end;
    OriginMap::const_iterator origin_begin, origin_end;
    const std::string* location_key;
    const scenario::NodeLocations* location_value;
    const std::string* origin_key;
    const scenario::ValueOrigin* origin_value;
    Sink measure_sink;
    std::string text_domain_temporary;
    Json::array_t array_domain_temporary;
    Json::object_t::Container object_domain_temporary;
    std::size_t text_max_size, array_max_size, object_max_size;
};
struct SinkCells {
    Sink* raw_this;
    std::string_view raw_value;
    Sink* character_this;
    char character_value;
    std::string_view character_view;
    Sink* string_this;
    std::string_view string_value;
    Sink* integer_this;
    std::int64_t integer_signed;
    std::uint64_t integer_unsigned;
    Decimal integer_decimal;
    Sink* hex_this;
    std::string_view hex_value;
    const char* hex_begin;
    const char* hex_end;
    char hex_character;
    unsigned char hex_byte;
    Sink* floating_this;
    double floating_value;
    std::uint64_t binary;
    std::array<char, 16> encoded;
    std::size_t encoded_index;
    Sink* location_sink;
    const scenario::SourceLocation* location_value;
    Sink* emit_sink;
    const View* emit_view;
    bool emit_comma;
    LocationMap::const_iterator location_begin, location_end;
    OriginMap::const_iterator origin_begin, origin_end;
    const std::string* location_key;
    const scenario::NodeLocations* location;
    const std::string* origin_key;
    const scenario::ValueOrigin* origin;
    const detail::State* view_input;
    View view_return;
};
struct ParserCells {
    Parser parser;
    Reader* literal_this;
    std::string_view literal_value;
    const Reader* next_this;
    char next_value;
    bool next_return;
    Reader* string_this;
    std::size_t string_start;
    std::string_view string_result, string_return;
    Reader* hex_this;
    std::string_view hex_result, hex_return;
    Parser* location_this;
    std::uint64_t offset, line, column;
    scenario::SourceLocation location_return;
    Parser* tree_this;
    std::size_t which, active, path, length, start, children, hex_index, duplicate_index;
    bool first;
    BuildFrame* parent;
    std::string_view key, tag, keys, scalar_view, object_name;
    Reader key_reader;
    Decimal index_decimal;
    unsigned byte;
    Json node;
    // basic_json::operator=(basic_json other): distinct by-value parameters.
    Json scalar_assignment_parameter, container_assignment_parameter, root_assignment_parameter;
    core::detail::JsonCleanup<Json> node_guard;
    Json::value_t type;
    bool boolean_value;
    std::int64_t signed_value;
    std::uint64_t unsigned_value, binary_value;
    double decoded_float;
    Json* attached;
    Json::array_t* array;
    Json::object_t* object;
    Json::object_t::Container* container;
    std::string decoded_key;
    BuildFrame assigned_frame;
    std::vector<std::string_view>::iterator heap_begin, heap_end, sort_begin, sort_end;
    Parser* metadata_this;
    std::string_view* previous_pointer;
    bool metadata_first;
    std::string_view metadata_key, metadata_return;
    Parser* parse_this;
    std::string_view previous, location_key, origin_key, name;
    scenario::SourceLocation location_value;
    std::optional<scenario::SourceLocation> mapping_key;
    scenario::NodeLocations node_location;
    scenario::ValueOriginKind origin_kind_value;
    std::optional<std::string_view> source;
    std::optional<std::string> decoded_source;
    scenario::ValueOrigin origin_value;
    Counts parse_return;
};
struct LocalCells {
    std::string* append_path;
    std::string_view append_token;
    const char* token_begin;
    const char* token_end;
    char append_character;
    const Json* pointer_root;
    std::string_view pointer;
    std::string* pointer_token;
    const Json* pointer_node;
    std::size_t pointer_at, pointer_index;
    char pointer_character;
    Json::const_iterator pointer_child, pointer_end;
    const Json* pointer_return;
    const View* source;
    const Counts* count;
    const Json* const* tree_begin;
    const Json* const* tree_end;
    const Json* tree;
    Json::const_iterator schema, schema_end;
    std::string path, token;
    OriginMap::const_iterator origin_begin, origin_end;
    const std::string* target;
    const scenario::ValueOrigin* origin;
    const Json* value;
    const Json* original;
    const Json* string_equal_value;
    std::string_view string_equal_text;
    const Json::string_t* string_equal_reference;
    bool string_equal_return;
};
struct InsertionCells {
    Json* object;
    std::string_view name;
    Json* child;
    Json::object_t* object_container;
    Json::object_t::Container* entries;
    Json replacement;
    core::detail::JsonCleanup<Json> replacement_guard;
    Json::object_t::Container* destination;
    std::size_t count;
    Json::object_t::Container::iterator begin, end;
    Json::object_t::value_type* entry;
    std::string moved_key, inserted_key;
    Json* state_root;
    core::detail::JsonOwner* retired_owner;
    Json* retired_json;
    core::detail::JsonOwner* owner_constructor_this;
    Json* owner_constructor_argument;
    core::detail::JsonOwner* owner_assignment_this;
    core::detail::JsonOwner* owner_assignment_argument;
    core::detail::JsonOwner* owner_assignment_return;
    const Json* clone_wrapper_argument;
    Json clone_return;
    Json owned_json_assignment_parameter;
};
struct OperationCells {
    PrimitiveCells primitive;
    CountCells counting;
    SinkCells sink;
    LocalCells local;
    InsertionCells insertion;
    std::array<MeasureFrameCells, 65> measure_frames;
    std::array<EmitFrameCells, 65> emit_frames;
    std::array<CloneFrameCells, 65> clone_frames;
    std::array<RetireFrameCells, 65> retire_frames;
    std::array<LocationFrameCells, 65> location_frames;
    std::array<EqualFrameCells, 65> equality_frames;
    const scenario::ScenarioDigestedPlan* plan;
    const scenario::MatrixExpandedScenario* matrix_return;
    const scenario::KernelDispatchedScenario* kernel_return;
    const scenario::MemoryPlacementResolvedScenario* memory_return;
    const scenario::ServiceCpuResolvedScenario* service_return;
    const scenario::ResourceResolvedScenario* resource_return;
    const scenario::DefaultExpandedScenario* defaults;
    const scenario::UnitNormalizedScenario* units;
    const scenario::StructurallyValidScenario* structure_return;
    const scenario::AuthoredScenario* authored;
    const scenario::ScenarioDigestedPlan* plan_getter_this;
    const scenario::MatrixExpandedScenario* matrix_getter_this;
    const scenario::KernelDispatchedScenario* kernel_getter_this;
    const scenario::MemoryPlacementResolvedScenario* memory_getter_this;
    const scenario::ServiceCpuResolvedScenario* service_getter_this;
    const scenario::ResourceResolvedScenario* resource_getter_this;
    const scenario::DefaultExpandedScenario* defaults_getter_this;
    const scenario::UnitNormalizedScenario* units_getter_this;
    const scenario::StructurallyValidScenario* structure_getter_this;
    const scenario::AuthoredScenario* authored_getter_this;
    std::array<const Json*, 3> tree_getter_returns;
    std::string_view name_getter_return;
    const LocationMap* locations_getter_return;
    const OriginMap* origins_getter_return;
    Limits limits;
    Counts measured;
    Usage used, usage_return;
    View source, output_view;
    std::unique_ptr<detail::State> state, constructor_state;
    Owner* constructor_this;
    Owner converting_owner;
    std::expected<Owner, Error> result;
    detail::State* state_pointer;
    std::size_t tree_index;
    Json cloned_tree;
    core::detail::JsonCleanup<Json> cloned_guard;
    core::detail::JsonOwner cloned_owner;
    const std::length_error* length_error;
    Error error;
    std::unexpected<Error> caught_error_return, capacity_error_return;
    const Counts* usage_count;
    bool usage_decode;
    std::string usage_string_domain;
    LocationMap usage_location_domain;
    OriginMap usage_origin_domain;
    std::vector<std::string_view> usage_index_domain;
    std::size_t string_max_size, location_max_size, origin_max_size, index_max_size;
    std::size_t retained, simultaneous;
    const Usage* fits_usage;
    const Limits* fits_limits;
    std::size_t fits_index;
    std::size_t Usage::* const* quantity_begin;
    std::size_t Usage::* const* quantity_end;
    std::size_t Usage::* quantity;
    detail::State* render_state;
    std::size_t render_size;
    Sink render_sink;
    std::string_view hash_input;
    core::Sha256Digest hash_return;
};
struct DecodeCells {
    OperationCells common;
    ParserCells parsing;
    std::string_view body;
    Limits limits;
    Counts counted, parsed;
    Usage used, parsed_usage;
    std::vector<std::string_view> keys;
    std::unique_ptr<detail::State> state;
    View output_view;
    Owner converting_owner;
    std::expected<Owner, Error> result;
    Parser parser_temporary;
    Error error;
    std::unexpected<Error> caught_error_return, capacity_error_return;
    const std::length_error* length_error;
};
auto usage(const Counts& count, bool decode) -> Usage {
    Usage result = count.usage;
    bound(result.source_name_bytes, std::string{}.max_size());
    bound(result.canonical_bytes, std::string{}.max_size());
    bound(result.location_rows, LocationMap{}.max_size());
    bound(result.origin_rows, OriginMap{}.max_size());
    bound(count.width, std::vector<std::string_view>{}.max_size());
    result.index_entries = decode ? count.width : 0;
    result.pointer_scratch_bytes = count.path;
    std::size_t retained = add(sizeof(Owner), sizeof(detail::State));
    retained = add(retained, count.dynamic);
    retained = add(retained, result.tree_octets);
    retained = add(retained, result.source_name_bytes);
    retained = add(retained, result.metadata_octets);
    retained = add(retained, mul(result.location_rows, sizeof(LocationMap::value_type)));
    retained = add(retained, mul(result.origin_rows, sizeof(OriginMap::value_type)));
    retained = add(retained, result.canonical_bytes);
    result.retained_bytes = retained;
    std::size_t simultaneous = add(add(sizeof(ProbeCells), decode ? sizeof(DecodeCells) : sizeof(OperationCells)), retained);
    simultaneous = add(simultaneous, add(count.dynamic, result.tree_octets));
    simultaneous = add(simultaneous, mul(2, count.path));
    simultaneous = add(simultaneous, mul(2, count.maximum));
    simultaneous = add(simultaneous, mul(result.index_entries, sizeof(std::string_view)));
    result.consumer_simultaneous_bytes = simultaneous;
    return result;
}
void fits(const Usage& used, const Limits& limits) {
    for (std::size_t i = 0; i < used.tree_nodes.size(); ++i) bound(used.tree_nodes[i], limits.tree_nodes[i]);
    for (const auto member : quantities) bound(used.*member, limits.*member);
}
void render(detail::State& state, std::size_t size) {
    state.canonical.reserve(size);
    Sink sink{&state.canonical};
    emit(sink, view(state));
    require(sink.size == size);
    state.digest = core::sha256(state.canonical);
}
} // namespace

auto detail::capture_fixed_stock() noexcept -> std::size_t { return sizeof(ProbeCells) + sizeof(OperationCells); }
auto detail::decode_fixed_stock() noexcept -> std::size_t { return sizeof(ProbeCells) + sizeof(DecodeCells); }
Owner::Owner(std::unique_ptr<detail::State> state) noexcept : state_(std::move(state)) {}
Owner::Owner(Owner&&) noexcept = default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
Owner::~Owner() = default;
auto Owner::authored() const noexcept -> const Json* { return state_ ? &state_->trees[0].value : nullptr; }
auto Owner::unit_normalized() const noexcept -> const Json* { return state_ ? &state_->trees[1].value : nullptr; }
auto Owner::default_expanded() const noexcept -> const Json* { return state_ ? &state_->trees[2].value : nullptr; }
auto Owner::locations() const noexcept -> const LocationMap* { return state_ ? &state_->locations : nullptr; }
auto Owner::origins() const noexcept -> const OriginMap* { return state_ ? &state_->origins : nullptr; }
auto Owner::source_name() const noexcept -> std::string_view { return state_ ? std::string_view{state_->name} : std::string_view{}; }
auto Owner::canonical() const noexcept -> std::string_view { return state_ ? std::string_view{state_->canonical} : std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_ ? &state_->digest : nullptr; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_ ? std::optional{state_->origin} : std::nullopt; }
auto Owner::usage() const noexcept -> Usage { return state_ ? state_->usage : Usage{}; }
auto measure_capture(const scenario::ScenarioDigestedPlan& plan, Limits limits) -> std::expected<Usage, Error> {
    try {
        valid_limits(limits);
        require(!plan.canonical_scenario_json().empty() &&
                !plan.canonical_resolved_scenario_plan_json().empty(), Error::state);

        const auto& defaults = plan.matrix_expanded().kernel_dispatched().memory_placement_resolved()
                                   .service_cpu_resolved().resource_resolved().default_expanded();
        const auto& units = defaults.unit_normalized();
        const auto& authored = units.structurally_valid().authored();
        const View source{{&authored.value(), &units.value(), &defaults.value()}, authored.source_name(),
                          &authored.locations(), &defaults.origins()};
        const auto measured = measure(source, limits);
        const auto used = ::epycbench::results::document_ancestry_serialization::usage(measured, false);
        fits(used, limits);
        return used;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto measure_decode(std::string_view body, Limits limits) -> std::expected<Usage, Error> {
    try {
        valid_limits(limits);
        const auto counted = Parser{{body}, &limits, nullptr, nullptr, {}, {}}.parse();
        const auto used = ::epycbench::results::document_ancestry_serialization::usage(counted, true);
        fits(used, limits);
        return used;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::capture(const scenario::ScenarioDigestedPlan& plan, Limits limits) -> std::expected<Owner, Error> {
    try {
        valid_limits(limits);
        require(!plan.canonical_scenario_json().empty() &&
                !plan.canonical_resolved_scenario_plan_json().empty(), Error::state);

        const auto& defaults = plan.matrix_expanded().kernel_dispatched().memory_placement_resolved()
                                   .service_cpu_resolved().resource_resolved().default_expanded();
        const auto& units = defaults.unit_normalized();
        const auto& authored = units.structurally_valid().authored();
        const View source{{&authored.value(), &units.value(), &defaults.value()}, authored.source_name(),
                          &authored.locations(), &defaults.origins()};
        const auto measured = measure(source, limits);
        const auto used = ::epycbench::results::document_ancestry_serialization::usage(measured, false);
        fits(used, limits);
        local(source, measured);
        auto state = std::make_unique<detail::State>();
        for (std::size_t i = 0; i < 3; ++i)
            state->trees[i] = core::detail::JsonOwner{core::detail::clone_json(*source.trees[i])};
        state->name = source.name;
        state->locations = *source.locations;
        state->origins = *source.origins;
        render(*state, used.canonical_bytes);
        state->origin = Origin::plan_captured;
        state->usage = used;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view body, Limits limits) -> std::expected<Owner, Error> {
    try {
        valid_limits(limits);
        const auto counted = Parser{{body}, &limits, nullptr, nullptr, {}, {}}.parse();
        const auto used = ::epycbench::results::document_ancestry_serialization::usage(counted, true);
        fits(used, limits);
        std::vector<std::string_view> keys;
        keys.reserve(used.index_entries);
        auto state = std::make_unique<detail::State>();
        const auto parsed = Parser{{body}, &limits, state.get(), &keys, {}, {}}.parse();
        require(::epycbench::results::document_ancestry_serialization::usage(parsed, true) == used);
        local(view(*state), parsed);
        render(*state, used.canonical_bytes);
        require(state->canonical == body, Error::encoding);
        state->origin = Origin::decoded_unassociated;
        state->usage = used;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
} // namespace epycbench::results::document_ancestry_serialization
