#include "results/host_system_controls.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::host_controls {
namespace {
constexpr std::array<std::string_view, 5> names{
    "smt_control", "smt_active", "numa_balancing", "thp_top_level_enabled", "thp_defrag"};
constexpr std::array<std::string_view, 4> states{"not_attempted", "available", "unavailable", "failed"};
constexpr std::array<std::string_view, 6> reasons{
    "not_supported", "source_missing", "access_denied", "read_failed", "malformed_source", "unsupported_value"};
constexpr std::array<std::string_view, 6> smt{"on", "off", "forceoff", "notsupported", "notimplemented", "threads"};
constexpr std::array<std::string_view, 3> enabled{"always", "madvise", "never"};
constexpr std::array<std::string_view, 5> defrag{"always", "defer", "defer+madvise", "madvise", "never"};
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
    std::array<char, 100> bytes{};
    std::size_t size{};
    Id(std::string_view run, std::string_view ordinal, std::string_view slot = {}) {
        for (auto part : {run, slot.empty() ? std::string_view{":"} : std::string_view{":system-controls:"}, ordinal,
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
void validate(const Input& input) {
    if (input.run.text().size() != 36) throw Reject{"identity_unavailable"};
    index(input.producer, 2);
    if (input.topology && (!input.topology->digest() || !input.topology->fingerprints()))
        throw Reject{"identity_unavailable"};
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& slot = input.observations[i];
        index(slot.state, states.size());
        if (slot.reason) index(*slot.reason, reasons.size());
        if (slot.observed_at && slot.observed_at->text().size() != 30) throw Reject{"identity_unavailable"};
        if (slot.state == State::not_attempted) {
            require(slot.value.index() == 0 && !slot.observed_at && !slot.reason && !slot.failure_event);
            continue;
        }
        require(slot.observed_at != nullptr);
        if (slot.state == State::available) {
            require(slot.value.index() == i + 1 && !slot.reason && !slot.failure_event);
            if (i == 0) {
                const auto& control = std::get<SmtControl>(slot.value);
                index(control.mode, smt.size());
                if (control.mode == SmtMode::threads) require(control.threads_per_core && *control.threads_per_core > 0);
                else require(!control.threads_per_core);
            } else if (i == 2) require(std::get<std::uint32_t>(slot.value) <= 3);
            else if (i == 3) index(std::get<ThpEnabled>(slot.value), enabled.size());
            else if (i == 4) index(std::get<ThpDefrag>(slot.value), defrag.size());
        } else {
            require(slot.value.index() == 0 && slot.reason.has_value());
            const auto reason = index(*slot.reason, reasons.size());
            if (slot.state == State::unavailable) require(reason < 3 && !slot.failure_event);
            else require(reason >= 3 && slot.failure_event.has_value());
        }
    }
}
struct Counter {
    std::size_t bytes{}, depth{};
    std::array<std::size_t, 5> entries{};
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
    std::array<nlohmann::json*, 5> stack{};
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
template <class Sink> void project(Sink& sink, const Input& input) {
    const Decimal ordinal{input.ordinal};
    sink.begin({}, false);
    sink.text("schema", profile); sink.text("run_id", input.run.text()); sink.text("ordinal", ordinal.text());
    sink.text("producer", input.producer == Producer::controller ? "controller" : "runner");
    if (input.topology) {
        sink.begin("topology", false);
        sink.text("canonical_sha256", Hex{*input.topology->digest(), false}.text());
        sink.text("physical_fingerprint", Hex{input.topology->fingerprints()->physical.digest(), true}.text());
        sink.text("effective_fingerprint", Hex{input.topology->fingerprints()->effective.digest(), true}.text());
        sink.end();
    } else sink.null("topology");
    sink.begin("observations", true);
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& slot = input.observations[i];
        sink.begin({}, false);
        sink.text("id", Id{input.run.text(), ordinal.text(), names[i]}.text());
        sink.text("slot", names[i]); sink.text("state", states[index(slot.state, states.size())]);
        if (slot.observed_at) sink.text("observed_at", slot.observed_at->text()); else sink.null("observed_at");
        if (slot.reason) sink.text("reason", reasons[index(*slot.reason, reasons.size())]); else sink.null("reason");
        if (slot.failure_event) sink.text("failure_event", Id{input.run.text(), Decimal{*slot.failure_event}.text()}.text());
        else sink.null("failure_event");
        if (slot.state != State::available) sink.null("value");
        else if (i == 0) {
            const auto& control = std::get<SmtControl>(slot.value);
            sink.begin("value", false);
            sink.text("mode", smt[index(control.mode, smt.size())]);
            if (control.threads_per_core) sink.number("threads_per_core", *control.threads_per_core);
            else sink.null("threads_per_core");
            sink.end();
        } else if (i == 1) sink.boolean("value", std::get<bool>(slot.value));
        else if (i == 2) sink.number("value", std::get<std::uint32_t>(slot.value));
        else if (i == 3) sink.text("value", enabled[index(std::get<ThpEnabled>(slot.value), enabled.size())]);
        else sink.text("value", defrag[index(std::get<ThpDefrag>(slot.value), defrag.size())]);
        sink.end();
    }
    sink.end(); sink.end();
}
auto failure(const char* code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.host_controls." + std::string{code}},
            .message = "Supplied system-control admission rejected", .operation = "retain_host_controls",
            .os_error = std::nullopt, .context = nlohmann::json::object()};
}
} // namespace
Owner::Owner(Owner&& other) noexcept
    : live_(std::exchange(other.live_, false)), run_(std::move(other.run_)), producer_(other.producer_), ordinal_(other.ordinal_),
      observations_(std::move(other.observations_)), topology_(std::move(other.topology_)),
      canonical_(std::move(other.canonical_)), digest_(other.digest_), usage_(std::exchange(other.usage_, {})) {
    other.canonical_.clear();
}
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false); run_ = std::move(other.run_); producer_ = other.producer_; ordinal_ = other.ordinal_;
        observations_ = std::move(other.observations_); topology_ = std::move(other.topology_);
        canonical_ = std::move(other.canonical_); digest_ = other.digest_;
        usage_ = std::exchange(other.usage_, {}); other.canonical_.clear();
    }
    return *this;
}
auto Owner::observations() const noexcept -> std::span<const Observation> {
    return live_ ? std::span<const Observation>{observations_} : std::span<const Observation>{};
}
auto Owner::topology() const noexcept -> const TopologyBinding* { return live_ && topology_ ? &*topology_ : nullptr; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return live_ ? &digest_ : nullptr; }
auto retain(const Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        validate(input);
        Counter counter;
        project(counter, input);
        Usage usage{5, sizeof(Owner), counter.bytes};
        add(usage.retained_bytes, input.run.text().size()); add(usage.retained_bytes, counter.bytes);
        const Decimal ordinal{input.ordinal};
        for (std::size_t i = 0; i < 5; ++i) {
            const auto& slot = input.observations[i];
            add(usage.retained_bytes, Id{input.run.text(), ordinal.text(), names[i]}.size);
            if (slot.observed_at) add(usage.retained_bytes, slot.observed_at->text().size());
            if (slot.failure_event) add(usage.retained_bytes, Id{input.run.text(), Decimal{*slot.failure_event}.text()}.size);
        }
        if (limits.observations < 5 || limits.retained_bytes < usage.retained_bytes || limits.canonical_bytes < counter.bytes)
            throw Reject{"capacity"};
        Owner owner;
        owner.run_ = input.run.text(); owner.producer_ = input.producer; owner.ordinal_ = input.ordinal; owner.usage_ = usage;
        if (input.topology) owner.topology_.emplace(*input.topology->digest(), *input.topology->fingerprints());
        for (std::size_t i = 0; i < 5; ++i) {
            const auto& slot = input.observations[i];
            auto& result = owner.observations_[i];
            result.id = Id{input.run.text(), ordinal.text(), names[i]}.text();
            result.state = slot.state; result.value = slot.value; result.reason = slot.reason;
            if (slot.observed_at) result.observed_at = slot.observed_at->text();
            if (slot.failure_event) result.failure_event = Id{input.run.text(), Decimal{*slot.failure_event}.text()}.text();
        }
        Tree tree;
        project(tree, input);
        auto canonical = core::canonicalize_json(tree.root);
        core::detail::ExpectedErrorCleanup cleanup{canonical};
        if (!canonical) throw Reject{"invalid_input"};
        owner.canonical_ = std::move(*canonical); owner.digest_ = core::sha256(owner.canonical_);
        return owner;
    } catch (const Reject& rejected) { return std::unexpected{failure(rejected.code)}; }
}
} // namespace epycbench::results::host_controls
