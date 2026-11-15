#include "results/pu_cpu_identity.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/topology/serialization.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::pu_cpu_identity {
namespace {
constexpr std::array<std::string_view, 2> names{"signature", "microcode"};
constexpr std::array<std::string_view, 4> states{"not_attempted", "available", "unavailable", "failed"};
constexpr std::array<std::string_view, 6> reasons{
    "not_supported", "source_missing", "access_denied", "read_failed", "malformed_source", "unsupported_value"};
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
    std::array<char, 128> bytes{};
    std::size_t size{};
    Id(std::string_view run, std::string_view ordinal, std::string_view cpu = {}, std::string_view slot = {}) {
        for (auto part : {run, slot.empty() ? std::string_view{":"} : std::string_view{":cpu-identity:"}, ordinal,
                          slot.empty() ? std::string_view{} : std::string_view{":"}, cpu,
                          slot.empty() ? std::string_view{} : std::string_view{":"}, slot})
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
struct WordHex {
    std::array<char, 10> bytes{'0', 'x'};
    explicit WordHex(std::uint32_t value) {
        constexpr std::string_view digits = "0123456789abcdef";
        for (std::size_t i = 0; i < 8; ++i) bytes[9 - i] = digits[(value >> (4 * i)) & 15U];
    }
    auto text() const -> std::string_view { return {bytes.data(), bytes.size()}; }
};
auto decode(Signature value) -> DecodedSignature {
    const auto base = (value.eax >> 8U) & 15U;
    const auto family = base == 15 ? base + ((value.eax >> 20U) & 255U) : base;
    require(family <= 255);
    const auto model = ((value.eax >> 4U) & 15U) + (base == 15 ? ((value.eax >> 16U) & 15U) * 16 : 0);
    return {value.eax, family, model, value.eax & 15U};
}
void validate_slot(const Slot& slot, std::size_t kind) {
    index(slot.state, states.size());
    if (slot.reason) index(*slot.reason, reasons.size());
    if (slot.observed_at && slot.observed_at->text().size() != 30) throw Reject{"identity_unavailable"};
    if (slot.state == State::not_attempted) {
        require(slot.value.index() == 0 && !slot.observed_at && !slot.reason && !slot.failure_event);
    } else {
        require(slot.observed_at != nullptr);
        if (slot.state == State::available) {
            require(slot.value.index() == kind + 1 && !slot.reason && !slot.failure_event);
            if (kind == 0) (void)decode(std::get<Signature>(slot.value));
        } else {
            require(slot.value.index() == 0 && slot.reason.has_value());
            const auto reason = index(*slot.reason, reasons.size());
            if (slot.state == State::unavailable) require(reason < 3 && !slot.failure_event);
            else require(reason >= 3 && slot.failure_event.has_value());
        }
    }
}
void preflight(const Input& input, Limits limits) {
    if (limits.processing_units > 1024 || limits.observations > 2048) throw Reject{"invalid_limits"};
    if (input.run.text().size() != 36 || !input.topology.digest() || !input.topology.fingerprints())
        throw Reject{"identity_unavailable"};
    index(input.producer, 2);
    if (input.rows.size() > 1024 || input.rows.size() > limits.processing_units ||
        input.rows.size() * 2 > limits.observations || limits.retained_bytes < sizeof(Owner) ||
        limits.canonical_bytes == 0) throw Reject{"capacity"};
    std::size_t lower_stock = sizeof(Owner);
    add(lower_stock, input.rows.size() * sizeof(OwnedRow));
    add(lower_stock, input.run.text().size());
    for (const auto& row : input.rows) {
        add(lower_stock, row.stable_pu_id.value().size());
        for (const auto* slot : {&row.signature, &row.microcode}) {
            if (slot->observed_at) add(lower_stock, slot->observed_at->text().size());
            if (slot->failure_event) add(lower_stock, Id{input.run.text(), Decimal{*slot->failure_event}.text()}.size);
        }
        add(lower_stock, Id{input.run.text(), Decimal{input.ordinal}.text(), Decimal{row.linux_cpu.value()}.text(), names[0]}.size);
        add(lower_stock, Id{input.run.text(), Decimal{input.ordinal}.text(), Decimal{row.linux_cpu.value()}.text(), names[1]}.size);
        if (row.stable_pu_id.value().empty()) throw Reject{"identity_unavailable"};
        require(row.stable_pu_id.value().size() <= 13);
        validate_slot(row.signature, 0); validate_slot(row.microcode, 1);
    }
    if (lower_stock > limits.retained_bytes) throw Reject{"capacity"};
}
struct Associated { const Row* input; bool online; };
template <class Sink> void project(Sink& sink, const Input& input,
                                   std::span<const Associated> rows, std::string_view unrepresented) {
    const Decimal ordinal{input.ordinal};
    sink.begin({}, false);
    sink.text("schema", profile); sink.text("run_id", input.run.text()); sink.text("ordinal", ordinal.text());
    sink.text("producer", input.producer == Producer::controller ? "controller" : "runner");
    sink.begin("topology", false);
    sink.text("canonical_sha256", Hex{*input.topology.digest(), false}.text());
    sink.text("physical_fingerprint", Hex{input.topology.fingerprints()->physical.digest(), true}.text());
    sink.text("effective_fingerprint", Hex{input.topology.fingerprints()->effective.digest(), true}.text());
    sink.end();
    sink.text("unrepresented_present_cpus", unrepresented);
    sink.begin("processing_units", true);
    for (const auto& item : rows) {
        const auto& row = *item.input;
        const Decimal cpu{row.linux_cpu.value()};
        sink.begin({}, false);
        sink.text("stable_pu_id", row.stable_pu_id.value()); sink.number("linux_cpu", row.linux_cpu.value());
        sink.boolean("topology_online", item.online);
        const std::array<const Slot*, 2> slots{&row.signature, &row.microcode};
        for (std::size_t i = 0; i < 2; ++i) {
            const auto& slot = *slots[i];
            sink.begin(names[i], false);
            sink.text("id", Id{input.run.text(), ordinal.text(), cpu.text(), names[i]}.text());
            sink.text("state", states[index(slot.state, states.size())]);
            if (slot.observed_at) sink.text("observed_at", slot.observed_at->text()); else sink.null("observed_at");
            if (slot.reason) sink.text("reason", reasons[index(*slot.reason, reasons.size())]); else sink.null("reason");
            if (slot.failure_event) sink.text("failure_event", Id{input.run.text(), Decimal{*slot.failure_event}.text()}.text());
            else sink.null("failure_event");
            if (slot.state != State::available) sink.null("value");
            else {
                sink.begin("value", false);
                if (i == 0) {
                    const auto value = decode(std::get<Signature>(slot.value));
                    sink.text("eax", WordHex{value.eax}.text()); sink.number("family", value.family);
                    sink.number("model", value.model); sink.number("stepping", value.stepping);
                } else sink.text("revision", WordHex{std::get<Microcode>(slot.value).revision}.text());
                sink.end();
            }
            sink.end();
        }
        sink.end();
    }
    sink.end(); sink.end();
}
auto failure(const char* code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.pu_cpu_identity." + std::string{code}},
            .message = "Supplied PU identity rejected", .operation = "retain_pu_cpu_identity",
            .os_error = std::nullopt, .context = nlohmann::json::object()};
}
} // namespace
Owner::Owner(Owner&& other) noexcept
    : live_(std::exchange(other.live_, false)), run_(std::move(other.run_)), producer_(other.producer_), ordinal_(other.ordinal_), rows_(std::move(other.rows_)),
      topology_(std::move(other.topology_)), unrepresented_(std::move(other.unrepresented_)),
      canonical_(std::move(other.canonical_)), digest_(other.digest_), usage_(std::exchange(other.usage_, {})) {
    other.canonical_.clear(); other.unrepresented_.clear();
}
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false); run_ = std::move(other.run_); producer_ = other.producer_; ordinal_ = other.ordinal_; rows_ = std::move(other.rows_);
        topology_ = std::move(other.topology_); unrepresented_ = std::move(other.unrepresented_);
        canonical_ = std::move(other.canonical_); digest_ = other.digest_;
        usage_ = std::exchange(other.usage_, {}); other.canonical_.clear(); other.unrepresented_.clear();
    }
    return *this;
}
auto Owner::rows() const noexcept -> std::span<const OwnedRow> { return live_ ? std::span<const OwnedRow>{rows_} : std::span<const OwnedRow>{}; }
auto Owner::topology() const noexcept -> const TopologyBinding* { return live_ && topology_ ? &*topology_ : nullptr; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return live_ ? &digest_ : nullptr; }
auto retain(const Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        preflight(input, limits);
#ifdef EPYCBENCH_PU_CPU_IDENTITY_TESTING
        ++testing::reload_calls;
#endif
        auto loaded = topology::deserialize_topology(input.topology.canonical(), topology::TopologyJsonInputPolicy::require_canonical);
        core::detail::ExpectedErrorCleanup loaded_cleanup{loaded};
        if (!loaded || loaded->fingerprints != *input.topology.fingerprints() ||
            loaded->canonical_json != input.topology.canonical() || core::sha256(loaded->canonical_json) != *input.topology.digest())
            throw Reject{"topology_mismatch"};
        const auto& pus = loaded->topology.processing_units();
        if (pus.size() != input.rows.size()) throw Reject{"topology_mismatch"};
        std::vector<Associated> rows;
        rows.reserve(input.rows.size());
        for (const auto& row : input.rows) rows.push_back({&row, loaded->topology.machine().online_cpus.contains(row.linux_cpu)});
        std::ranges::sort(rows, {}, [](const auto& row) { return row.input->linux_cpu.value(); });
        core::CpuSet represented;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = *rows[i].input;
            if (i && rows[i - 1].input->linux_cpu == row.linux_cpu) throw Reject{"topology_mismatch"};
            const auto handle = loaded->stable_ids.processing_unit(row.stable_pu_id);
            const auto pu = std::ranges::find(pus, row.linux_cpu, &topology::ProcessingUnit::linux_cpu_id);
            if (!handle || pu == pus.end() || pu->id != *handle) throw Reject{"topology_mismatch"};
            represented = represented.set_union(core::CpuSet{row.linux_cpu});
        }
        const auto unrepresented = loaded->topology.machine().present_cpus.difference(represented).to_range_list();
        Usage usage{rows.size(), rows.size() * 2, 0, sizeof(Owner)};
        add(usage.retained_bytes, rows.size() * sizeof(OwnedRow));
        add(usage.retained_bytes, input.run.text().size()); add(usage.retained_bytes, unrepresented.size());
        const Decimal ordinal{input.ordinal};
        for (const auto& item : rows) {
            const auto& row = *item.input;
            const Decimal cpu{row.linux_cpu.value()};
            add(usage.retained_bytes, row.stable_pu_id.value().size());
            const std::array<const Slot*, 2> slots{&row.signature, &row.microcode};
            for (std::size_t i = 0; i < 2; ++i) {
                const auto& slot = *slots[i];
                add(usage.retained_bytes, Id{input.run.text(), ordinal.text(), cpu.text(), names[i]}.size);
                if (slot.observed_at) add(usage.retained_bytes, slot.observed_at->text().size());
                if (slot.failure_event) add(usage.retained_bytes, Id{input.run.text(), Decimal{*slot.failure_event}.text()}.size);
            }
        }
        Counter counter;
        project(counter, input, rows, unrepresented);
        usage.canonical_bytes = counter.bytes; add(usage.retained_bytes, counter.bytes);
        if (counter.bytes > limits.canonical_bytes || usage.retained_bytes > limits.retained_bytes) throw Reject{"capacity"};
#ifdef EPYCBENCH_PU_CPU_IDENTITY_TESTING
        ++testing::output_calls;
#endif
        Owner owner;
        owner.run_ = input.run.text(); owner.producer_ = input.producer; owner.ordinal_ = input.ordinal; owner.unrepresented_ = unrepresented; owner.usage_ = usage;
        owner.topology_.emplace(*input.topology.digest(), *input.topology.fingerprints());
        owner.rows_.reserve(rows.size());
        for (const auto& item : rows) {
            const auto& row = *item.input;
            owner.rows_.push_back({std::string{row.stable_pu_id.value()}, row.linux_cpu, item.online, {}, {}});
            auto& result = owner.rows_.back();
            const std::array<const Slot*, 2> slots{&row.signature, &row.microcode};
            const std::array<Observation*, 2> destinations{&result.signature, &result.microcode};
            const Decimal cpu{row.linux_cpu.value()};
            for (std::size_t i = 0; i < 2; ++i) {
                const auto& slot = *slots[i]; auto& destination = *destinations[i];
                destination.id = Id{input.run.text(), ordinal.text(), cpu.text(), names[i]}.text();
                destination.state = slot.state; destination.reason = slot.reason;
                if (slot.state == State::available) {
                    if (i == 0) destination.value = decode(std::get<Signature>(slot.value));
                    else destination.value = std::get<Microcode>(slot.value);
                }
                if (slot.observed_at) destination.observed_at = slot.observed_at->text();
                if (slot.failure_event) destination.failure_event = Id{input.run.text(), Decimal{*slot.failure_event}.text()}.text();
            }
        }
        Tree tree;
        project(tree, input, rows, unrepresented);
        auto canonical = core::canonicalize_json(tree.root);
        core::detail::ExpectedErrorCleanup cleanup{canonical};
        if (!canonical) throw Reject{"invalid_input"};
        owner.canonical_ = std::move(*canonical); owner.digest_ = core::sha256(owner.canonical_);
        return owner;
    } catch (const Reject& rejected) { return std::unexpected{failure(rejected.code)}; }
}
} // namespace epycbench::results::pu_cpu_identity
