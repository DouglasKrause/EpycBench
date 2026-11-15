#include "results/anonymous_thp_policy_owner.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::anonymous_thp_policy_owner {
namespace {
constexpr std::array<std::string_view, 4> enumeration_states{"not_attempted", "complete", "unavailable", "failed"};
constexpr std::array<std::string_view, 4> states{"not_attempted", "available", "unavailable", "failed"};
constexpr std::array<std::string_view, 4> modes{"always", "madvise", "never", "inherit"};
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
template <class Fact> void validate_fact(const Fact& fact) {
    const auto state = index(fact.state, 4);
    if (fact.reason) index(*fact.reason, reasons.size());
    if (fact.observed_at && fact.observed_at->text().size() != 30) throw Reject{"identity_unavailable"};
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
}
template <class Fact> void count_strings(Usage& usage, const Input& input, const Fact& fact) {
    if (fact.observed_at) add(usage.retained_bytes, fact.observed_at->text().size());
    if (fact.failure_event) add(usage.retained_bytes, Id{input.run.text(), *fact.failure_event}.size);
}
auto preflight(const Input& input, Limits limits) -> Usage {
    if (limits.rows > 54) throw Reject{"invalid_limits"};
    if (input.run.text().size() != 36 || (input.topology && (!input.topology->digest() || !input.topology->fingerprints())))
        throw Reject{"identity_unavailable"};
    index(input.producer, 2);
    if (input.rows.size() > limits.rows) throw Reject{"capacity"};
    validate_fact(input.enumeration);
    if (input.enumeration.state == EnumerationState::not_attempted || input.enumeration.state == EnumerationState::unavailable)
        require(input.rows.empty());
    Usage usage{input.rows.size(), 0, sizeof(Owner)};
    add(usage.retained_bytes, input.rows.size() * sizeof(OwnedRow));
    add(usage.retained_bytes, input.run.text().size()); count_strings(usage, input, input.enumeration);
    for (const auto& row : input.rows) {
        require(row.size.value() >= 1024 && std::has_single_bit(row.size.value()));
        validate_fact(row);
        if (row.mode) index(*row.mode, modes.size());
        require(row.mode.has_value() == (row.state == State::available));
        count_strings(usage, input, row);
    }
    if (usage.retained_bytes > limits.retained_bytes || limits.canonical_bytes == 0) throw Reject{"capacity"};
    return usage;
}
template <class Sink, class Fact> void project_fact(Sink& sink, const Input& input, const Fact& fact, std::string_view state) {
    sink.text("state", state);
    if (fact.observed_at) sink.text("observed_at", fact.observed_at->text()); else sink.null("observed_at");
    if (fact.reason) sink.text("reason", reasons[index(*fact.reason, reasons.size())]); else sink.null("reason");
    if (fact.failure_event) sink.text("failure_event", Id{input.run.text(), *fact.failure_event}.text()); else sink.null("failure_event");
}
template <class Sink> void project(Sink& sink, const Input& input, std::span<const RowInput* const> rows) {
    sink.begin({}, false); sink.text("schema", profile); sink.text("run_id", input.run.text());
    sink.text("ordinal", Decimal{input.ordinal}.text()); sink.text("producer", input.producer == Producer::controller ? "controller" : "runner");
    if (input.topology) {
        sink.begin("topology", false);
        sink.text("canonical_sha256", Hex{*input.topology->digest(), false}.text());
        sink.text("physical_fingerprint", Hex{input.topology->fingerprints()->physical.digest(), true}.text());
        sink.text("effective_fingerprint", Hex{input.topology->fingerprints()->effective.digest(), true}.text()); sink.end();
    } else sink.null("topology");
    sink.begin("enumeration", false);
    project_fact(sink, input, input.enumeration, enumeration_states[index(input.enumeration.state, enumeration_states.size())]); sink.end();
    sink.begin("sizes", true);
    for (const auto* row : rows) {
        sink.begin({}, false); sink.text("size_bytes", Decimal{row->size.value()}.text());
        project_fact(sink, input, *row, states[index(row->state, states.size())]);
        if (row->mode) sink.text("mode", modes[index(*row->mode, modes.size())]); else sink.null("mode");
        sink.end();
    }
    sink.end(); sink.end();
}
template <class Destination, class Fact> void copy_fact(Destination& destination, const Input& input, const Fact& fact) {
    destination.state = fact.state; destination.reason = fact.reason;
    if (fact.observed_at) destination.observed_at = fact.observed_at->text();
    if (fact.failure_event) destination.failure_event = Id{input.run.text(), *fact.failure_event}.text();
}
auto failure(const char* code) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.anonymous_thp_policy_owner." + std::string{code}},
            .message = "Supplied anonymous THP policy inventory rejected", .operation = "retain_anonymous_thp_policy_inventory",
            .os_error = std::nullopt, .context = nlohmann::json::object()};
}
} // namespace
Owner::Owner(Owner&& other) noexcept
    : live_(std::exchange(other.live_, false)), run_(std::move(other.run_)), producer_(other.producer_), ordinal_(other.ordinal_), enumeration_(std::move(other.enumeration_)),
      rows_(std::move(other.rows_)), topology_(std::move(other.topology_)), canonical_(std::move(other.canonical_)),
      digest_(other.digest_), usage_(std::exchange(other.usage_, {})) { other.canonical_.clear(); }
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false); run_ = std::move(other.run_); producer_ = other.producer_; ordinal_ = other.ordinal_; enumeration_ = std::move(other.enumeration_);
        rows_ = std::move(other.rows_); topology_ = std::move(other.topology_); canonical_ = std::move(other.canonical_);
        digest_ = other.digest_; usage_ = std::exchange(other.usage_, {}); other.canonical_.clear();
    }
    return *this;
}
auto Owner::enumeration() const noexcept -> const Enumeration* { return live_ ? &enumeration_ : nullptr; }
auto Owner::rows() const noexcept -> std::span<const OwnedRow> { return live_ ? std::span<const OwnedRow>{rows_} : std::span<const OwnedRow>{}; }
auto Owner::topology() const noexcept -> const TopologyBinding* { return live_ && topology_ ? &*topology_ : nullptr; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return live_ ? &digest_ : nullptr; }
auto retain(const Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        auto usage = preflight(input, limits);
        std::array<const RowInput*, 54> storage{};
        auto rows = std::span{storage}.first(input.rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i] = &input.rows[i];
        std::ranges::sort(rows, {}, [](const auto* row) { return row->size; });
        require(std::ranges::adjacent_find(rows, {}, [](const auto* row) { return row->size; }) == rows.end());
        Counter counter; project(counter, input, rows);
        usage.canonical_bytes = counter.bytes; add(usage.retained_bytes, counter.bytes);
        if (usage.canonical_bytes > limits.canonical_bytes || usage.retained_bytes > limits.retained_bytes) throw Reject{"capacity"};
#ifdef EPYCBENCH_ANONYMOUS_THP_POLICY_OWNER_TESTING
        ++testing::output_calls;
#endif
        Owner owner; owner.run_ = input.run.text(); owner.producer_ = input.producer; owner.ordinal_ = input.ordinal; owner.usage_ = usage;
        if (input.topology) owner.topology_.emplace(*input.topology->digest(), *input.topology->fingerprints());
        copy_fact(owner.enumeration_, input, input.enumeration); owner.rows_.reserve(rows.size());
        for (const auto* row : rows) {
            owner.rows_.push_back({row->size, row->state, row->mode, {}, row->reason, {}});
            copy_fact(owner.rows_.back(), input, *row);
        }
        Tree tree; project(tree, input, rows);
        auto canonical = core::canonicalize_json(tree.root); core::detail::ExpectedErrorCleanup cleanup{canonical};
        if (!canonical) throw Reject{"invalid_input"};
        owner.canonical_ = std::move(*canonical); owner.digest_ = core::sha256(owner.canonical_);
        return owner;
    } catch (const Reject& rejected) { return std::unexpected{failure(rejected.code)}; }
}
} // namespace epycbench::results::anonymous_thp_policy_owner
