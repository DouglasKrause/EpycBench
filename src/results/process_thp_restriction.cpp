#include "results/process_thp_restriction.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <array>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::process_thp_restriction {
namespace {
constexpr std::array<std::string_view, 2> roles{"controller", "runner"};
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
auto preflight(const Input& input, Limits limits) -> Usage {
    if (input.run.text().size() != 36 || (input.topology && (!input.topology->digest() || !input.topology->fingerprints())))
        throw Reject{"identity_unavailable"};
    require(index(input.producer, roles.size()) == index(input.subject.role, roles.size()));
    const auto& fact = input.observation;
    const auto state = index(fact.state, states.size());
    if (fact.reason) index(*fact.reason, reasons.size());
    if (fact.value) index(*fact.value, 2);
    if (fact.observed_at && fact.observed_at->text().size() != 30) throw Reject{"identity_unavailable"};
    require(fact.value.has_value() == (state == 1));
    if (state == 0) require(!fact.observed_at && !fact.reason && !fact.failure_event);
    else {
        require(fact.observed_at != nullptr);
        if (state == 1) require(!fact.reason && !fact.failure_event);
        else {
            require(fact.reason.has_value());
            if (state == 2) require(index(*fact.reason, reasons.size()) < 3 && !fact.failure_event);
            else require(index(*fact.reason, reasons.size()) >= 3 && fact.failure_event);
        }
    }
    Usage usage{1, 0, sizeof(Owner)};
    add(usage.retained_bytes, input.run.text().size());
    add(usage.retained_bytes, Id{input.run.text(), input.subject.lifecycle_event_sequence}.size);
    if (fact.observed_at) add(usage.retained_bytes, fact.observed_at->text().size());
    if (fact.failure_event) add(usage.retained_bytes, Id{input.run.text(), *fact.failure_event}.size);
    if (limits.observations < 1 || limits.canonical_bytes == 0 || usage.retained_bytes > limits.retained_bytes) throw Reject{"capacity"};
    return usage;
}
template <class Sink> void project(Sink& sink, const Input& input) {
    sink.begin({}, false); sink.text("schema", profile); sink.text("run_id", input.run.text());
    sink.text("ordinal", Decimal{input.ordinal}.text()); sink.text("producer", roles[index(input.producer, roles.size())]);
    sink.begin("subject", false); sink.text("role", roles[index(input.subject.role, roles.size())]);
    sink.text("event_id", Id{input.run.text(), input.subject.lifecycle_event_sequence}.text()); sink.end();
    if (input.topology) {
        sink.begin("topology", false);
        sink.text("canonical_sha256", Hex{*input.topology->digest(), false}.text());
        sink.text("physical_fingerprint", Hex{input.topology->fingerprints()->physical.digest(), true}.text());
        sink.text("effective_fingerprint", Hex{input.topology->fingerprints()->effective.digest(), true}.text()); sink.end();
    } else sink.null("topology");
    const auto& fact = input.observation;
    sink.begin("observation", false); sink.text("state", states[index(fact.state, states.size())]);
    if (fact.value) sink.boolean("disabled", *fact.value == Restriction::disabled); else sink.null("disabled");
    if (fact.observed_at) sink.text("observed_at", fact.observed_at->text()); else sink.null("observed_at");
    if (fact.reason) sink.text("reason", reasons[index(*fact.reason, reasons.size())]); else sink.null("reason");
    if (fact.failure_event) sink.text("failure_event", Id{input.run.text(), *fact.failure_event}.text()); else sink.null("failure_event");
    sink.end(); sink.end();
}
auto failure(const char* code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.process_thp_restriction." + std::string{code}},
            .message = "Supplied process THP restriction rejected", .operation = "retain_process_thp_restriction",
            .os_error = std::nullopt, .context = nlohmann::json::object()};
}
} // namespace
Owner::Owner(Owner&& other) noexcept
    : live_(std::exchange(other.live_, false)), run_(std::move(other.run_)), producer_(other.producer_), ordinal_(other.ordinal_), subject_(std::move(other.subject_)),
      observation_(std::move(other.observation_)), topology_(std::move(other.topology_)), canonical_(std::move(other.canonical_)),
      digest_(other.digest_), usage_(std::exchange(other.usage_, {})) { other.canonical_.clear(); }
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false); run_ = std::move(other.run_); producer_ = other.producer_; ordinal_ = other.ordinal_; subject_ = std::move(other.subject_);
        observation_ = std::move(other.observation_); topology_ = std::move(other.topology_); canonical_ = std::move(other.canonical_);
        digest_ = other.digest_; usage_ = std::exchange(other.usage_, {}); other.canonical_.clear();
    }
    return *this;
}
auto Owner::subject() const noexcept -> const Subject* { return live_ ? &subject_ : nullptr; }
auto Owner::observation() const noexcept -> const Observation* { return live_ ? &observation_ : nullptr; }
auto Owner::topology() const noexcept -> const TopologyBinding* { return live_ && topology_ ? &*topology_ : nullptr; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return live_ ? &digest_ : nullptr; }
auto retain(const Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        auto usage = preflight(input, limits);
        Counter counter; project(counter, input);
        usage.canonical_bytes = counter.bytes; add(usage.retained_bytes, counter.bytes);
        if (usage.canonical_bytes > limits.canonical_bytes || usage.retained_bytes > limits.retained_bytes) throw Reject{"capacity"};
#ifdef EPYCBENCH_PROCESS_THP_RESTRICTION_TESTING
        ++testing::output_calls;
#endif
        Owner owner; owner.run_ = input.run.text(); owner.producer_ = input.producer; owner.ordinal_ = input.ordinal; owner.usage_ = usage;
        owner.subject_ = {input.subject.role, std::string{Id{input.run.text(), input.subject.lifecycle_event_sequence}.text()}};
        if (input.topology) owner.topology_.emplace(*input.topology->digest(), *input.topology->fingerprints());
        const auto& fact = input.observation;
        owner.observation_.state = fact.state; owner.observation_.value = fact.value; owner.observation_.reason = fact.reason;
        if (fact.observed_at) owner.observation_.observed_at = fact.observed_at->text();
        if (fact.failure_event) owner.observation_.failure_event = Id{input.run.text(), *fact.failure_event}.text();
        Tree tree; project(tree, input);
        auto canonical = core::canonicalize_json(tree.root); core::detail::ExpectedErrorCleanup cleanup{canonical};
        if (!canonical) throw Reject{"invalid_input"};
        owner.canonical_ = std::move(*canonical); owner.digest_ = core::sha256(owner.canonical_);
        return owner;
    } catch (const Reject& rejected) { return std::unexpected{failure(rejected.code)}; }
}
} // namespace epycbench::results::process_thp_restriction
