#include "results/frequency_policy_owner.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/serialization.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::frequency_policy_owner {
namespace {
constexpr std::array<std::string_view, 3> states{"not_attempted", "unavailable", "failed"};
constexpr std::array<std::string_view, 6> reasons{
    "not_supported", "source_missing", "access_denied", "read_failed", "malformed_source", "unsupported_value"};
constexpr std::array<std::string_view, 2> sources{"hardware_current", "scaling_current"};
struct Reject { const char* code; };
void require(bool condition) { if (!condition) throw Reject{"invalid_input"}; }
template <class E> auto index(E value, std::size_t size) -> std::size_t {
    const auto result = static_cast<std::size_t>(value);
    require(result < size);
    return result;
}
void add(std::size_t& value, std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - value) throw Reject{"capacity"};
    value += amount;
}
struct Decimal {
    std::array<char, 20> bytes{};
    std::size_t size{};
    explicit Decimal(std::uint64_t value) {
        size = static_cast<std::size_t>(std::to_chars(bytes.data(), bytes.data() + bytes.size(), value).ptr - bytes.data());
    }
    auto text() const -> std::string_view { return {bytes.data(), size}; }
};
struct Id {
    std::array<char, 57> bytes{};
    std::size_t size{};
    Id(std::string_view run, std::uint64_t sequence) {
        const Decimal decimal{sequence};
        for (auto part : {run, std::string_view{":"}, decimal.text()})
            for (char c : part) bytes[size++] = c;
    }
    auto text() const -> std::string_view { return {bytes.data(), size}; }
};
struct Hex {
    std::array<char, 71> bytes{};
    std::size_t size{};
    Hex(const core::Sha256Digest& digest, bool prefixed) {
        if (prefixed) for (char c : std::string_view{"sha256:"}) bytes[size++] = c;
        constexpr std::string_view digits = "0123456789abcdef";
        for (auto byte : digest.bytes()) {
            const auto value = std::to_integer<unsigned>(byte);
            bytes[size++] = digits[value >> 4U]; bytes[size++] = digits[value & 15U];
        }
    }
    auto text() const -> std::string_view { return {bytes.data(), size}; }
};
struct Counter {
    std::size_t bytes{}, depth{};
    std::array<std::size_t, 6> entries{};
    void entry(std::string_view key) {
        if (!depth) return;
        if (entries[depth - 1]++) add(bytes, 1);
        if (!key.empty()) { add(bytes, key.size()); add(bytes, 3); }
    }
    void begin(std::string_view key, bool) { entry(key); add(bytes, 1); entries[depth++] = 0; }
    void end() { --depth; add(bytes, 1); }
    void text(std::string_view key, std::string_view value) { entry(key); add(bytes, value.size()); add(bytes, 2); }
    void null(std::string_view key) { entry(key); add(bytes, 4); }
    void boolean(std::string_view key, bool value) { entry(key); add(bytes, value ? 4 : 5); }
    void number(std::string_view key, std::uint32_t value) { entry(key); add(bytes, Decimal{value}.size); }
};
struct Tree {
    nlohmann::json root;
    core::detail::JsonCleanup<nlohmann::json> cleanup{root};
    std::array<nlohmann::json*, 6> stack{};
    std::size_t depth{};
    auto slot(std::string_view key) -> nlohmann::json& {
        if (!depth) return root;
        auto& parent = *stack[depth - 1];
        if (parent.is_array()) { parent.push_back(nullptr); return parent.back(); }
        return parent[std::string{key}];
    }
    void begin(std::string_view key, bool array) {
        auto& value = slot(key);
        value = array ? nlohmann::json::array() : nlohmann::json::object();
        stack[depth++] = &value;
    }
    void end() { --depth; }
    void text(std::string_view key, std::string_view value) { auto& child = slot(key); child = std::string{value}; }
    void null(std::string_view key) { slot(key) = nullptr; }
    void boolean(std::string_view key, bool value) { slot(key) = value; }
    void number(std::string_view key, std::uint32_t value) { slot(key) = value; }
};
struct OctetHex {
    std::array<char, 8192> bytes{};
    std::size_t size{};
    explicit OctetHex(std::string_view value) {
        constexpr std::string_view digits = "0123456789abcdef";
        for (char c : value) {
            const auto byte = static_cast<unsigned char>(c);
            bytes[size++] = digits[byte >> 4U]; bytes[size++] = digits[byte & 15U];
        }
    }
    auto text() const -> std::string_view { return {bytes.data(), size}; }
};
void validate_uncovered(const UncoveredInput& row) {
    index(row.state, states.size());
    if (row.reason) index(*row.reason, reasons.size());
    if (row.observed_at && row.observed_at->text().size() != 30) throw Reject{"identity_unavailable"};
    if (row.state == State::not_attempted) require(!row.observed_at && !row.reason && !row.failure_event);
    else {
        require(row.observed_at && row.reason);
        if (row.state == State::unavailable) require(index(*row.reason, reasons.size()) < 3 && !row.failure_event);
        else require(index(*row.reason, reasons.size()) >= 3 && row.failure_event);
    }
}
auto preflight(const Input& input, Limits limits) -> Usage {
    if (limits.policies > 16384 || limits.memberships > 16384 || limits.uncovered > 16384 || limits.label_bytes > 4096)
        throw Reject{"invalid_limits"};
    if (input.run.text().size() != 36 || !input.topology.digest() || !input.topology.fingerprints())
        throw Reject{"identity_unavailable"};
    index(input.producer, 2);
    if (input.policies.size() > limits.policies || input.uncovered.size() > limits.uncovered ||
        limits.retained_bytes < sizeof(Owner) || limits.canonical_bytes == 0) throw Reject{"capacity"};
    Usage usage{input.policies.size(), 0, input.uncovered.size(), 0, 0, 0, sizeof(Owner)};
    add(usage.retained_bytes, input.run.text().size());
    add(usage.retained_bytes, input.policies.size() * sizeof(Policy));
    add(usage.retained_bytes, input.uncovered.size() * sizeof(Uncovered));
    for (const auto& row : input.policies) {
        const auto& metadata = row.metadata;
        if (row.observed_at.text().size() != 30 || metadata.domain_id().cpus().empty()) throw Reject{"identity_unavailable"};
        index(metadata.source_kind(), sources.size());
        add(usage.memberships, metadata.domain_id().cpus().size());
        if (usage.memberships > limits.memberships) throw Reject{"capacity"};
        require(std::ranges::is_sorted(metadata.domain_id().cpus()) &&
                std::ranges::adjacent_find(metadata.domain_id().cpus()) == metadata.domain_id().cpus().end());
        require(metadata.scaling_minimum().value() != 0 && metadata.scaling_minimum() <= metadata.scaling_maximum());
        for (auto label : {metadata.scaling_driver().value(), metadata.scaling_governor().value()}) {
            if (label.size() > limits.label_bytes) throw Reject{"capacity"};
            require(!label.empty() && std::ranges::none_of(label, [](char c) {
                const auto byte = static_cast<unsigned char>(c); return byte < 0x20U || byte == 0x7fU;
            }));
            usage.label_bytes = std::max(usage.label_bytes, label.size());
            add(usage.total_label_bytes, label.size());
            if (usage.total_label_bytes > limits.total_label_bytes) throw Reject{"capacity"};
        }
        add(usage.retained_bytes, row.observed_at.text().size());
    }
    if (usage.memberships > 16384 - usage.uncovered) throw Reject{"capacity"};
    add(usage.retained_bytes, usage.memberships * sizeof(Association));
    add(usage.retained_bytes, usage.total_label_bytes);
    for (const auto& row : input.uncovered) {
        validate_uncovered(row);
        if (row.observed_at) add(usage.retained_bytes, row.observed_at->text().size());
        if (row.failure_event) add(usage.retained_bytes, Id{input.run.text(), *row.failure_event}.size);
    }
    if (usage.retained_bytes > limits.retained_bytes) throw Reject{"capacity"};
    return usage;
}
struct AssociationView { core::LinuxCpuId cpu; std::optional<std::string_view> stable; bool online; };
auto association(std::span<const AssociationView> values, core::LinuxCpuId cpu) -> const AssociationView& {
    const auto found = std::ranges::lower_bound(values, cpu, {}, &AssociationView::cpu);
    if (found == values.end() || found->cpu != cpu) throw Reject{"topology_mismatch"};
    return *found;
}
template <class Sink> void project_association(Sink& sink, const AssociationView& value) {
    sink.number("linux_cpu", value.cpu.value());
    if (value.stable) sink.text("stable_pu_id", *value.stable); else sink.null("stable_pu_id");
    sink.boolean("topology_online", value.online);
}
template <class Sink> void project(Sink& sink, const Input& input, std::span<const PolicyInput* const> policies,
                                   std::span<const UncoveredInput* const> uncovered, std::span<const AssociationView> cpus) {
    sink.begin({}, false);
    sink.text("schema", profile); sink.text("run_id", input.run.text()); sink.text("ordinal", Decimal{input.ordinal}.text());
    sink.text("producer", input.producer == Producer::controller ? "controller" : "runner");
    sink.begin("topology", false);
    sink.text("canonical_sha256", Hex{*input.topology.digest(), false}.text());
    sink.text("physical_fingerprint", Hex{input.topology.fingerprints()->physical.digest(), true}.text());
    sink.text("effective_fingerprint", Hex{input.topology.fingerprints()->effective.digest(), true}.text()); sink.end();
    sink.begin("policies", true);
    for (std::size_t i = 0; i < policies.size(); ++i) {
        const auto& row = *policies[i]; const auto& m = row.metadata;
        sink.begin({}, false); sink.number("index", static_cast<std::uint32_t>(i)); sink.text("observed_at", row.observed_at.text());
        sink.text("source_kind", sources[index(m.source_kind(), sources.size())]);
        for (auto item : {std::pair{std::string_view{"scaling_driver"},m.scaling_driver().value()},
                          std::pair{std::string_view{"scaling_governor"},m.scaling_governor().value()}}) {
            sink.begin(item.first, false); sink.text("encoding", "hex"); sink.text("octets", OctetHex{item.second}.text());
            sink.text("size_bytes", Decimal{item.second.size()}.text()); sink.end();
        }
        sink.text("scaling_minimum_hz", Decimal{m.scaling_minimum().value()}.text());
        sink.text("scaling_maximum_hz", Decimal{m.scaling_maximum().value()}.text());
        sink.begin("related_cpus", true);
        for (auto cpu : m.domain_id().cpus()) { sink.begin({}, false); project_association(sink, association(cpus, cpu)); sink.end(); }
        sink.end(); sink.begin("topology_online_cpus", true);
        for (auto cpu : m.domain_id().cpus()) if (association(cpus, cpu).online) sink.number({}, cpu.value());
        sink.end(); sink.end();
    }
    sink.end(); sink.begin("uncovered_cpus", true);
    for (const auto* row : uncovered) {
        sink.begin({}, false); project_association(sink, association(cpus, row->linux_cpu));
        sink.text("state", states[index(row->state, states.size())]);
        if (row->observed_at) sink.text("observed_at", row->observed_at->text()); else sink.null("observed_at");
        if (row->reason) sink.text("reason", reasons[index(*row->reason, reasons.size())]); else sink.null("reason");
        if (row->failure_event) sink.text("failure_event", Id{input.run.text(), *row->failure_event}.text()); else sink.null("failure_event");
        sink.end();
    }
    sink.end(); sink.end();
}
auto copy_association(const AssociationView& value) -> Association {
    return {value.cpu, value.stable ? std::optional<std::string>{*value.stable} : std::nullopt, value.online};
}
auto failure(const char* code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.frequency_policy_owner." + std::string{code}},
            .message = "Supplied frequency policy metadata rejected", .operation = "retain_frequency_policy_metadata",
            .os_error = std::nullopt, .context = nlohmann::json::object()};
}
} // namespace
Owner::Owner(Owner&& other) noexcept
    : live_(std::exchange(other.live_, false)), run_(std::move(other.run_)), producer_(other.producer_), ordinal_(other.ordinal_), policies_(std::move(other.policies_)),
      uncovered_(std::move(other.uncovered_)), topology_(std::move(other.topology_)), canonical_(std::move(other.canonical_)),
      digest_(other.digest_), usage_(std::exchange(other.usage_, {})) { other.canonical_.clear(); }
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false); run_ = std::move(other.run_); producer_ = other.producer_; ordinal_ = other.ordinal_; policies_ = std::move(other.policies_);
        uncovered_ = std::move(other.uncovered_); topology_ = std::move(other.topology_); canonical_ = std::move(other.canonical_);
        digest_ = other.digest_; usage_ = std::exchange(other.usage_, {}); other.canonical_.clear();
    }
    return *this;
}
auto Owner::policies() const noexcept -> std::span<const Policy> { return live_ ? std::span<const Policy>{policies_} : std::span<const Policy>{}; }
auto Owner::uncovered_cpus() const noexcept -> std::span<const Uncovered> { return live_ ? std::span<const Uncovered>{uncovered_} : std::span<const Uncovered>{}; }
auto Owner::topology() const noexcept -> const TopologyBinding* { return live_ && topology_ ? &*topology_ : nullptr; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return live_ ? &digest_ : nullptr; }
auto retain(const Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        auto usage = preflight(input, limits);
#ifdef EPYCBENCH_FREQUENCY_POLICY_OWNER_TESTING
        ++testing::reload_calls;
#endif
        auto loaded = topology::deserialize_topology(input.topology.canonical(), topology::TopologyJsonInputPolicy::require_canonical);
        core::detail::ExpectedErrorCleanup loaded_cleanup{loaded};
        if (!loaded || loaded->fingerprints != *input.topology.fingerprints() || loaded->canonical_json != input.topology.canonical() ||
            core::sha256(loaded->canonical_json) != *input.topology.digest()) throw Reject{"topology_mismatch"};
        std::vector<const PolicyInput*> policies;
        policies.reserve(input.policies.size()); for (const auto& row : input.policies) policies.push_back(&row);
        std::ranges::sort(policies, [](const auto* a, const auto* b) { return a->metadata.domain_id() < b->metadata.domain_id(); });
        std::vector<const UncoveredInput*> uncovered;
        uncovered.reserve(input.uncovered.size()); for (const auto& row : input.uncovered) uncovered.push_back(&row);
        std::ranges::sort(uncovered, {}, [](const auto* row) { return row->linux_cpu; });
        std::vector<core::LinuxCpuId> supplied;
        supplied.reserve(usage.memberships + usage.uncovered);
        for (const auto* row : policies) for (auto cpu : row->metadata.domain_id().cpus()) supplied.push_back(cpu);
        for (const auto* row : uncovered) supplied.push_back(row->linux_cpu);
        std::ranges::sort(supplied);
        const auto& machine = loaded->topology.machine();
        if (supplied.size() != machine.present_cpus.cardinality() ||
            std::ranges::adjacent_find(supplied) != supplied.end() || !std::ranges::equal(supplied, machine.present_cpus))
            throw Reject{"topology_mismatch"};
        std::vector<AssociationView> represented;
        represented.reserve(loaded->topology.processing_units().size());
        for (const auto& pu : loaded->topology.processing_units()) {
            const auto id = loaded->stable_ids.processing_unit_id(pu.id);
            if (!id) throw Reject{"topology_mismatch"};
            represented.push_back({pu.linux_cpu_id, id->get().value(), machine.online_cpus.contains(pu.linux_cpu_id)});
        }
        std::ranges::sort(represented, {}, &AssociationView::cpu);
        std::vector<AssociationView> cpus;
        cpus.reserve(supplied.size());
        auto next = represented.begin();
        for (auto cpu : supplied) {
            if (next != represented.end() && next->cpu == cpu) { cpus.push_back(*next); ++next; }
            else cpus.push_back({cpu, {}, machine.online_cpus.contains(cpu)});
        }
        for (const auto& cpu : cpus) if (cpu.stable) add(usage.retained_bytes, cpu.stable->size());
        for (const auto* row : policies) for (auto cpu : row->metadata.domain_id().cpus())
            if (association(cpus, cpu).online) add(usage.retained_bytes, sizeof(core::LinuxCpuId));
        Counter counter; project(counter, input, policies, uncovered, cpus);
        usage.canonical_bytes = counter.bytes; add(usage.retained_bytes, counter.bytes);
        if (usage.canonical_bytes > limits.canonical_bytes || usage.retained_bytes > limits.retained_bytes) throw Reject{"capacity"};
#ifdef EPYCBENCH_FREQUENCY_POLICY_OWNER_TESTING
        ++testing::output_calls;
#endif
        Owner owner; owner.run_ = input.run.text(); owner.producer_ = input.producer; owner.ordinal_ = input.ordinal; owner.usage_ = usage;
        owner.topology_.emplace(*input.topology.digest(), *input.topology.fingerprints());
        owner.policies_.reserve(policies.size()); owner.uncovered_.reserve(uncovered.size());
        for (const auto* row : policies) {
            const auto& m = row->metadata;
            owner.policies_.push_back({std::string{row->observed_at.text()}, m.source_kind(), std::string{m.scaling_driver().value()},
                                      std::string{m.scaling_governor().value()}, m.scaling_minimum(), m.scaling_maximum(), {}, {}});
            auto& result = owner.policies_.back(); result.related_cpus.reserve(m.domain_id().cpus().size());
            result.topology_online_cpus.reserve(m.domain_id().cpus().size());
            for (auto cpu : m.domain_id().cpus()) {
                const auto& value = association(cpus, cpu); result.related_cpus.push_back(copy_association(value));
                if (value.online) result.topology_online_cpus.push_back(cpu);
            }
        }
        for (const auto* row : uncovered) {
            owner.uncovered_.push_back({copy_association(association(cpus, row->linux_cpu)), row->state, {}, row->reason, {}});
            auto& result = owner.uncovered_.back();
            if (row->observed_at) result.observed_at = row->observed_at->text();
            if (row->failure_event) result.failure_event = Id{input.run.text(), *row->failure_event}.text();
        }
        Tree tree; project(tree, input, policies, uncovered, cpus);
        auto canonical = core::canonicalize_json(tree.root); core::detail::ExpectedErrorCleanup cleanup{canonical};
        if (!canonical) throw Reject{"invalid_input"};
        owner.canonical_ = std::move(*canonical); owner.digest_ = core::sha256(owner.canonical_);
        return owner;
    } catch (const Reject& rejected) { return std::unexpected{failure(rejected.code)}; }
}
} // namespace epycbench::results::frequency_policy_owner
