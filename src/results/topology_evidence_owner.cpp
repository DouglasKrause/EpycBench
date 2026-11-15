#include "results/topology_evidence_owner.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/topology/serialization.hpp"
#include "topology/topology_json_parser.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <utility>
#include <vector>

namespace epycbench::results::topology_evidence {
namespace {
using Json = nlohmann::json;
struct Rejected { std::string_view code; };
void require(bool condition) { if (!condition) throw Rejected{"invalid_topology"}; }
void bounded(std::size_t value, std::size_t limit) {
    if (value > limit) throw Rejected{"capacity"};
}
void add(std::size_t& value, std::size_t amount, std::size_t limit) {
    if (value > limit || amount > limit - value) throw Rejected{"capacity"};
    value += amount;
}
auto sum(std::size_t a, std::size_t b) -> std::size_t {
    add(a, b, std::numeric_limits<std::size_t>::max());
    return a;
}
auto times(std::size_t a, std::size_t b) -> std::size_t {
    if (a && b > std::numeric_limits<std::size_t>::max() / a) throw Rejected{"capacity"};
    return a * b;
}
constexpr auto members = std::to_array<std::size_t Limits::*>({
    &Limits::bytes, &Limits::tokens, &Limits::depth, &Limits::string_token,
    &Limits::scalar_token, &Limits::objects, &Limits::processing_units, &Limits::numa_nodes,
    &Limits::groups, &Limits::array_entries, &Limits::expanded_ranges, &Limits::ancestry,
    &Limits::derived_ids, &Limits::retained});
auto error(std::string_view code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.topology." + std::string{code}},
            .message = "Supplied topology admission rejected", .operation = "retain_topology",
            .os_error = std::nullopt, .context = Json::object()};
}
auto scan(std::string_view text, Limits limits) -> Usage {
    for (const auto member : members)
        if (limits.*member > supported_limits().*member) throw Rejected{"invalid_limits"};
    Usage usage;
    usage.bytes = text.size();
    bounded(usage.bytes, limits.bytes);
    usage.retained = sizeof(Owner);
    add(usage.retained, text.size(), limits.retained);
    std::array<char, 32> stack{};
    std::size_t depth = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = text[i++];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '/')
            throw Rejected{"noncanonical"};
        if (c == '{' || c == '[') {
            add(usage.tokens, 1, limits.tokens);
            bounded(depth + 1, limits.depth);
            stack[depth++] = c;
            usage.depth = std::max(usage.depth, depth);
        } else if (c == '}' || c == ']') {
            require(depth && stack[depth - 1] == (c == '}' ? '{' : '['));
            --depth;
        } else if (c == '"') {
            add(usage.tokens, 1, limits.tokens);
            const auto start = i;
            bool closed = false;
            while (i < text.size()) {
                const auto next = text[i++];
                if (next == '"') { closed = true; break; }
                require(static_cast<unsigned char>(next) >= 0x20);
                if (next == '\\') {
                    require(i < text.size());
                    ++i;
                }
                bounded(i - start, limits.string_token);
            }
            require(closed);
            const auto length = i - start - 1;
            bounded(length, limits.string_token);
            usage.string_token = std::max(usage.string_token, length);
        } else if (c != ',' && c != ':') {
            add(usage.tokens, 1, limits.tokens);
            const auto start = i - 1;
            while (i < text.size() && text[i] != ',' && text[i] != '}' && text[i] != ']') {
                require(text[i] != '{' && text[i] != '[' && text[i] != '"' && text[i] != ':');
                if (text[i] == ' ' || text[i] == '\n' || text[i] == '\r' || text[i] == '\t' || text[i] == '/')
                    throw Rejected{"noncanonical"};
                ++i;
                bounded(i - start, limits.scalar_token);
            }
            bounded(i - start, limits.scalar_token);
            usage.scalar_token = std::max(usage.scalar_token, i - start);
        }
    }
    require(depth == 0 && !text.empty());
    return usage;
}
auto field(const Json& value, std::string_view key) -> const Json& {
    require(value.is_object());
    const auto found = value.find(key);
    require(found != value.end());
    return *found;
}
auto text(const Json& value) -> std::string_view {
    require(value.is_string());
    return value.get_ref<const std::string&>();
}
auto array(const Json& value) -> const Json& { require(value.is_array()); return value; }
void arrays(const Json& value, Usage& usage, Limits limits) {
    if (value.is_array()) add(usage.array_entries, value.size(), limits.array_entries);
    if (value.is_structured()) for (const auto& child : value) arrays(child, usage, limits);
}
void range(const Json& value, Usage& usage, Limits limits) {
    auto source = text(value);
    if (source.empty()) return;
    while (!source.empty()) {
        const auto comma = source.find(',');
        const auto item = source.substr(0, comma);
        const auto dash = item.find('-');
        const auto number = [](std::string_view digits) {
            std::uint32_t result{};
            const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), result);
            require(!digits.empty() && ec == std::errc{} && end == digits.data() + digits.size());
            return result;
        };
        const auto first = number(item.substr(0, dash));
        const auto last = dash == std::string_view::npos ? first : number(item.substr(dash + 1));
        require(last >= first);
        const auto width = std::uint64_t{last} - first + 1;
        if (width > limits.expanded_ranges) throw Rejected{"capacity"};
        add(usage.expanded_ranges, static_cast<std::size_t>(width), limits.expanded_ranges);
        if (comma == std::string_view::npos) break;
        source.remove_prefix(comma + 1);
        require(!source.empty());
    }
}
void range_attribute(const Json& value, Usage& usage, Limits limits) {
    const auto state = text(field(value, "state"));
    if (state == "available") range(field(value, "value"), usage, limits);
    else if (state == "conflicted")
        for (const auto& alternative : array(field(value, "values")))
            range(field(alternative, "value"), usage, limits);
    else require(state == "unavailable" || state == "unknown");
}
struct Entry { std::string_view id; const Json* value; std::size_t members{}; };
using Index = std::vector<Entry>;
auto index(const Json& values) -> Index {
    Index result;
    result.reserve(values.size());
    for (const auto& value : values) result.push_back({text(field(value, "id")), &value, 0});
    std::ranges::sort(result, {}, &Entry::id);
    for (std::size_t i = 1; i < result.size(); ++i) require(result[i-1].id != result[i].id);
    return result;
}
auto lookup(Index& entries, std::string_view id) -> Entry& {
    const auto found = std::ranges::lower_bound(entries, id, {}, &Entry::id);
    require(found != entries.end() && found->id == id);
    return *found;
}
void preflight(const Json& root, Usage& usage, Limits limits) {
    arrays(root, usage, limits);
    const auto& objects = field(root, "objects");
    constexpr std::array<std::string_view, 8> names{
        "packages", "dies", "groups", "cores", "processing_units", "caches", "numa_nodes", "pci_functions"};
    for (const auto name : names) add(usage.objects, array(field(objects, name)).size(), limits.objects);
    const auto& pus = field(objects, "processing_units");
    const auto& groups = field(objects, "groups");
    const auto& nodes = field(objects, "numa_nodes");
    usage.processing_units = pus.size(); usage.groups = groups.size(); usage.numa_nodes = nodes.size();
    bounded(usage.processing_units, limits.processing_units);
    bounded(usage.groups, limits.groups);
    // The loader builds matrix.nodes from these NUMA objects, not a wire node list.
    bounded(usage.numa_nodes, limits.numa_nodes);
    const auto& machine = field(root, "machine");
    for (const auto name : {"present_cpus", "online_cpus", "present_nodes", "online_nodes"})
        range(field(machine, name), usage, limits);
    for (const auto name : {"allowed_cpus", "effective_cpus", "allowed_nodes", "effective_nodes"})
        range_attribute(field(machine, name), usage, limits);
    auto packages = index(field(objects, "packages"));
    auto cores = index(field(objects, "cores"));
    auto dies = index(field(objects, "dies"));
    auto pu_index = index(pus);
    auto group_index = index(groups);
    for (auto& core : cores) {
        (void)lookup(packages, text(field(*core.value, "package")));
        const auto& die = field(*core.value, "die");
        if (!die.is_null()) (void)lookup(dies, text(die));
    }
    for (auto& die : dies) (void)lookup(packages, text(field(*die.value, "package")));
    for (const auto& pu : pus) {
        auto& core = lookup(cores, text(field(pu, "core")));
        ++core.members;
        ++lookup(packages, text(field(*core.value, "package"))).members;
        const auto& die = field(*core.value, "die");
        if (!die.is_null()) ++lookup(dies, text(die)).members;
    }
    const auto membership = [&](const Json& object) {
        const auto& values = array(field(object, "processing_units"));
        for (const auto& id : values) (void)lookup(pu_index, text(id));
        return values.size();
    };
    const auto add_id = [&](std::size_t length) { add(usage.derived_ids, length, limits.derived_ids); };
    const auto simple = [&](std::size_t count) { return sum(128, times(11, count)); };
    add_id(128); // machine
    for (const auto& package : packages) add_id(simple(package.members));
    for (const auto* values : {&cores, &dies}) for (const auto& entry : *values)
        add_id(simple(sum(entry.members, lookup(packages, text(field(*entry.value, "package"))).members)));
    add_id(times(128, sum(pus.size(), sum(nodes.size(), field(objects, "pci_functions").size()))));
    for (const auto& cache : field(objects, "caches")) add_id(simple(membership(cache)));
    for (const auto& node : nodes) (void)membership(node);
    std::vector<std::size_t> lengths(groups.size()), depths(groups.size());
    std::vector<unsigned char> states(groups.size());
    const auto derive = [&](auto&& self, std::size_t i, std::size_t traversed) -> void {
        bounded(traversed, limits.ancestry);
        require(states[i] != 1);
        if (states[i] == 2) return;
        states[i] = 1;
        const auto& value = *group_index[i].value;
        const auto count = membership(value);
        const auto package_count = lookup(packages, text(field(value, "package"))).members;
        const auto& subtype = field(value, "subtype");
        const auto labels = sum(text(field(value, "kind")).size(), subtype.is_null() ? 0 : text(subtype).size());
        auto length = sum(simple(sum(package_count, count)), times(3, labels));
        std::size_t parent_length = 4;
        const auto& parent = field(value, "parent");
        if (!parent.is_null()) {
            const auto& found = lookup(group_index, text(parent));
            const auto parent_index = static_cast<std::size_t>(&found - group_index.data());
            self(self, parent_index, traversed + 1);
            parent_length = lengths[parent_index];
            depths[i] = depths[parent_index] + 1;
        }
        bounded(depths[i], limits.ancestry);
        usage.ancestry = std::max(usage.ancestry, depths[i]);
        length = sum(length, times(3, parent_length));
        add_id(length);
        lengths[i] = length;
        states[i] = 2;
    };
    for (std::size_t i = 0; i < group_index.size(); ++i) derive(derive, i, 0);
}
} // namespace
Owner::Owner(std::string bytes, topology::TopologyFingerprints identities, core::Sha256Digest digest, Usage usage)
    : bytes_(std::move(bytes)), identities_(identities), digest_(digest), usage_(usage) {}
Owner::Owner(Owner&& other) noexcept
    : bytes_(std::move(other.bytes_)), identities_(std::exchange(other.identities_, std::nullopt)),
      digest_(std::exchange(other.digest_, std::nullopt)), usage_(std::exchange(other.usage_, {})) {
    other.bytes_.clear();
}
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        bytes_ = std::move(other.bytes_);
        identities_ = std::exchange(other.identities_, std::nullopt);
        digest_ = std::exchange(other.digest_, std::nullopt);
        usage_ = std::exchange(other.usage_, {});
        other.bytes_.clear();
    }
    return *this;
}
auto retain(std::string_view input, Limits limits, std::optional<topology::TopologyFingerprints> expected)
    -> std::expected<Owner, core::Error> {
    try {
        auto usage = scan(input, limits);
        {
#ifdef EPYCBENCH_TOPOLOGY_OWNER_TESTING
            ++testing::dom_calls;
#endif
            auto parsed = topology::detail::parse_topology_json(input);
            core::detail::ExpectedErrorCleanup error_cleanup{parsed};
            if (!parsed) throw Rejected{"invalid_topology"};
            core::detail::JsonCleanup cleanup{*parsed};
            preflight(*parsed, usage, limits);
        }
#ifdef EPYCBENCH_TOPOLOGY_OWNER_TESTING
        ++testing::reload_calls;
#endif
        auto loaded = topology::deserialize_topology(input, topology::TopologyJsonInputPolicy::require_canonical);
        core::detail::ExpectedErrorCleanup cleanup{loaded};
        if (!loaded) throw Rejected{"invalid_topology"};
        if (expected && *expected != loaded->fingerprints) throw Rejected{"identity_mismatch"};
        const auto digest = core::sha256(loaded->canonical_json);
        return Owner{std::move(loaded->canonical_json), loaded->fingerprints, digest, usage};
    } catch (const Rejected& rejected) {
        return std::unexpected{error(rejected.code)};
    }
}
} // namespace epycbench::results::topology_evidence
