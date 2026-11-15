#include "results/validation_event_owner.hpp"
#include "core/json_ownership.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::validation_event {
namespace {
using namespace monitoring;
constexpr std::array<std::string_view, 4> channels{"page_faults", "context_switches", "temperature",
                                                   "frequency"};
constexpr std::array<std::string_view, 3> dispositions{"disabled", "optional", "required"};
constexpr std::array<std::string_view, 3> evaluations{"not_requested", "not_evaluated",
                                                      "evaluated"};
constexpr std::array<std::string_view, 5> outcomes{"pass", "fail", "warning", "unavailable",
                                                   "not_applicable"};
constexpr std::array<std::string_view, 5> effects{"none", "undetermined", "reject", "warn",
                                                  "disable"};
constexpr std::array<std::string_view, 3> effective{"active", "unsupported_warning", "disabled"};
constexpr std::array<std::string_view, 4> statuses{"accepted", "accepted_with_warnings",
                                                   "incomplete", "rejected"};
struct Reject {
    const char* code;
};
void add(std::size_t& n, std::size_t amount) {
    if (amount > std::numeric_limits<std::size_t>::max() - n)
        throw Reject{"overflow"};
    n += amount;
}
template <class E> auto index(E value, std::size_t count) -> std::size_t {
    const auto n = static_cast<std::size_t>(value);
    if (n >= count)
        throw Reject{"invalid_input"};
    return n;
}
struct Decimal {
    std::array<char, 20> bytes{};
    std::size_t size{};
    explicit Decimal(std::uint64_t value) {
        size = static_cast<std::size_t>(
            std::to_chars(bytes.data(), bytes.data() + bytes.size(), value).ptr - bytes.data());
    }
    auto text() const -> std::string_view { return {bytes.data(), size}; }
};
struct Derived {
    std::optional<Outcome> outcome;
    Effect effect;
    std::optional<MonitoringEffectiveChannelDisposition> effective;
};
auto derive(MonitoringChannelDisposition requested, const Slot& slot,
            OptionalUnavailableSourcePolicy policy) -> Derived {
    index(requested, 3);
    index(slot.evaluation, 3);
    index(policy, 3);
    if (slot.support)
        index(*slot.support, 2);
    if (slot.checked_at && slot.checked_at->text().size() != 30)
        throw Reject{"invalid_input"};
    if (requested == MonitoringChannelDisposition::disabled) {
        if (slot.evaluation != Evaluation::not_requested || slot.support || slot.checked_at)
            throw Reject{"invalid_input"};
        return {Outcome::not_applicable, Effect::none,
                MonitoringEffectiveChannelDisposition::disabled};
    }
    if (slot.evaluation == Evaluation::not_evaluated && !slot.support && !slot.checked_at)
        return {std::nullopt, Effect::undetermined, std::nullopt};
    if (slot.evaluation != Evaluation::evaluated || !slot.support)
        throw Reject{"invalid_input"};
    if (*slot.support == MonitoringSourceSupport::supported)
        return {Outcome::pass, Effect::none, MonitoringEffectiveChannelDisposition::active};
    if (requested == MonitoringChannelDisposition::required ||
        policy == OptionalUnavailableSourcePolicy::fail)
        return {Outcome::fail, Effect::reject, std::nullopt};
    if (policy == OptionalUnavailableSourcePolicy::warn)
        return {Outcome::warning, Effect::warn,
                MonitoringEffectiveChannelDisposition::unsupported_warning};
    return {Outcome::not_applicable, Effect::disable,
            MonitoringEffectiveChannelDisposition::disabled};
}
struct Counter {
    std::size_t bytes{};
    std::array<std::size_t, 8> entries{};
    std::size_t depth{};
    void entry(std::string_view key) {
        if (!depth)
            return;
        if (entries[depth - 1]++)
            add(bytes, 1);
        if (!key.empty()) {
            add(bytes, key.size());
            add(bytes, 3);
        }
    }
    void begin(std::string_view key, bool) {
        entry(key);
        add(bytes, 1);
        entries[depth++] = 0;
    }
    void end() {
        --depth;
        add(bytes, 1);
    }
    void text(std::string_view key, std::string_view v) {
        entry(key);
        add(bytes, v.size());
        add(bytes, 2);
    }
    void null(std::string_view key) {
        entry(key);
        add(bytes, 4);
    }
    void boolean(std::string_view key, bool v) {
        entry(key);
        add(bytes, v ? 4 : 5);
    }
};
struct Tree {
    nlohmann::json root;
    core::detail::JsonCleanup<nlohmann::json> cleanup{root};
    std::array<nlohmann::json*, 8> stack{};
    std::size_t depth{};
    auto slot(std::string_view key) -> nlohmann::json& {
        if (!depth)
            return root;
        auto& parent = *stack[depth - 1];
        if (parent.is_array()) {
            parent.push_back(nullptr);
            return parent.back();
        }
        // Insert an empty owner before constructing any child payload.
        return parent[std::string{key}];
    }
    void begin(std::string_view key, bool array) {
        auto& value = slot(key);
        value = array ? nlohmann::json::array() : nlohmann::json::object();
        stack[depth++] = &value;
    }
    void end() { --depth; }
    void text(std::string_view key, std::string_view v) {
        auto& value = slot(key);
        value = std::string{v};
    }
    void null(std::string_view key) { slot(key) = nullptr; }
    void boolean(std::string_view key, bool v) { slot(key) = v; }
};
// IDs contain only validated UUID bytes, decimal digits and fixed ASCII literals.
struct Id {
    std::array<char, 100> bytes{};
    std::size_t size{};
    Id(std::string_view run, std::string_view ordinal, std::string_view channel) {
        for (auto part :
             {run, std::string_view{":validation:"}, ordinal, std::string_view{":"}, channel})
            for (char c : part)
                bytes[size++] = c;
    }
    auto text() const -> std::string_view { return {bytes.data(), size}; }
};
template <class Sink>
void project(Sink& sink, const Input& input, const std::array<Derived, 4>& facts, Status status) {
    Decimal ordinal{input.ordinal};
    sink.begin({}, false);
    sink.text("schema", profile);
    sink.text("run_id", input.run.text());
    sink.text("assessment_ordinal", ordinal.text());
    sink.text("producer", input.producer == Producer::controller ? "controller" : "runner");
    sink.begin("policy", false);
    for (std::size_t i = 0; i < 4; ++i)
        sink.text(
            channels[i],
            dispositions[index(input.policy.disposition(static_cast<MonitoringChannel>(i)), 3)]);
    sink.boolean("before_observation", input.policy.before_observation());
    sink.boolean("after_observation", input.policy.after_observation());
    sink.boolean("during_observation", input.policy.during_observation());
    if (input.policy.during_interval())
        sink.text("during_interval_ns", Decimal{input.policy.during_interval()->value()}.text());
    else
        sink.null("during_interval_ns");
    constexpr std::array<std::string_view, 3> optional{"fail", "warn", "disable"};
    sink.text("optional_unsupported", optional[index(input.policy.optional_unsupported(), 3)]);
    sink.end();
    sink.begin("checks", true);
    for (std::size_t i = 0; i < 4; ++i) {
        sink.begin({}, false);
        sink.text("id", Id{input.run.text(), ordinal.text(), channels[i]}.text());
        std::array<char, 70> code{};
        std::size_t n = 0;
        for (auto part : {std::string_view{"preflight.monitoring."}, channels[i],
                          std::string_view{".capability"}})
            for (char c : part)
                code[n++] = c;
        sink.text("code", std::string_view{code.data(), n});
        sink.text("authority", "monitoring-acquisition-driver");
        sink.text("channel", channels[i]);
        sink.text(
            "requested",
            dispositions[index(input.policy.disposition(static_cast<MonitoringChannel>(i)), 3)]);
        sink.text("evaluation", evaluations[index(input.slots[i].evaluation, 3)]);
        if (input.slots[i].support)
            sink.text("support", *input.slots[i].support == MonitoringSourceSupport::supported
                                     ? "supported"
                                     : "unsupported");
        else
            sink.null("support");
        if (input.slots[i].checked_at)
            sink.text("checked_at", input.slots[i].checked_at->text());
        else
            sink.null("checked_at");
        if (facts[i].outcome)
            sink.text("outcome", outcomes[index(*facts[i].outcome, 5)]);
        else
            sink.null("outcome");
        if (facts[i].outcome)
            sink.null("absence_reason");
        else
            sink.text("absence_reason", "not_evaluated");
        sink.text("preparation_effect", effects[index(facts[i].effect, 5)]);
        if (facts[i].effective)
            sink.text("effective_channel", effective[index(*facts[i].effective, 3)]);
        else
            sink.null("effective_channel");
        sink.end();
    }
    sink.end();
    sink.text("capability_status", statuses[index(status, 4)]);
    sink.begin("warning_check_ids", true);
    for (std::size_t i = 0; i < 4; ++i)
        if (facts[i].outcome == Outcome::warning)
            sink.text({}, Id{input.run.text(), ordinal.text(), channels[i]}.text());
    sink.end();
    sink.end();
}
auto failure(const char* code) -> core::Error {
    return core::Error{.category = core::ErrorCategory::results,
                       .code =
                           core::StableErrorCode{std::string{"results.validation_event."} + code},
                       .message = "Supplied validation assessment rejected",
                       .operation = "retain_validation_event",
                       .os_error = std::nullopt,
                       .context = nullptr};
}
} // namespace
Owner::Owner(const MonitoringExecutionPolicy& policy) : policy_(policy) {}
Owner::Owner(Owner&& other) noexcept : policy_(other.policy_) { *this = std::move(other); }
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this != &other) {
        live_ = std::exchange(other.live_, false);
        run_ = std::move(other.run_);
        producer_ = other.producer_;
        ordinal_ = other.ordinal_;
        policy_ = other.policy_;
        checks_ = std::move(other.checks_);
        warnings_ = other.warnings_;
        warning_count_ = other.warning_count_;
        status_ = other.status_;
        canonical_ = std::move(other.canonical_);
        digest_ = other.digest_;
        usage_ = other.usage_;
    }
    return *this;
}
auto Owner::checks() const noexcept -> std::span<const Check> {
    return live_ ? std::span<const Check>{checks_} : std::span<const Check>{};
}
auto Owner::warning_indices() const noexcept -> std::span<const std::size_t> {
    return live_ ? std::span<const std::size_t>{warnings_.data(), warning_count_}
                 : std::span<const std::size_t>{};
}
auto Owner::policy() const noexcept -> const MonitoringExecutionPolicy* {
    return live_ ? &policy_ : nullptr;
}
auto Owner::status() const noexcept -> std::optional<Status> {
    return live_ ? std::optional{status_} : std::nullopt;
}
auto Owner::canonical() const noexcept -> std::string_view {
    return live_ ? std::string_view{canonical_} : std::string_view{};
}
auto Owner::digest() const noexcept -> const core::Sha256Digest* {
    return live_ ? &digest_ : nullptr;
}
auto Owner::usage() const noexcept -> Usage { return live_ ? usage_ : Usage{}; }
auto retain(const Input& input, Limits limits) -> std::expected<Owner, core::Error> {
    try {
        if (input.run.text().size() != 36)
            throw Reject{"invalid_input"};
        index(input.producer, 2);
        index(input.policy.optional_unsupported(), 3);
        std::array<Derived, 4> facts{};
        bool rejected = false, incomplete = false, warning = false;
        for (std::size_t i = 0; i < 4; ++i) {
            facts[i] = derive(input.policy.disposition(static_cast<MonitoringChannel>(i)),
                              input.slots[i], input.policy.optional_unsupported());
            rejected |= facts[i].outcome == Outcome::fail;
            incomplete |= !facts[i].outcome;
            warning |= facts[i].outcome == Outcome::warning;
        }
        const auto status = rejected     ? Status::rejected
                            : incomplete ? Status::incomplete
                            : warning    ? Status::accepted_with_warnings
                                         : Status::accepted;
        Counter count;
        project(count, input, facts, status);
        Usage usage{4, sizeof(Owner), count.bytes};
        add(usage.retained_bytes, input.run.text().size());
        add(usage.retained_bytes, count.bytes);
        Decimal ordinal{input.ordinal};
        for (std::size_t i = 0; i < 4; ++i) {
            add(usage.retained_bytes,
                Id{input.run.text(), ordinal.text(), channels[i]}.text().size());
            if (input.slots[i].checked_at)
                add(usage.retained_bytes, input.slots[i].checked_at->text().size());
        }
        if (limits.checks < 4 || limits.retained_bytes < usage.retained_bytes ||
            limits.canonical_bytes < count.bytes)
            throw Reject{"capacity"};
        Owner owner{input.policy};
        owner.run_ = input.run.text();
        owner.producer_ = input.producer;
        owner.ordinal_ = input.ordinal;
        owner.status_ = status;
        owner.usage_ = usage;
        for (std::size_t i = 0; i < 4; ++i) {
            owner.checks_[i] = {
                std::string{Id{input.run.text(), ordinal.text(), channels[i]}.text()},
                input.policy.disposition(static_cast<MonitoringChannel>(i)),
                input.slots[i].evaluation,
                input.slots[i].support,
                input.slots[i].checked_at
                    ? std::optional<std::string>{input.slots[i].checked_at->text()}
                    : std::nullopt,
                facts[i].outcome,
                facts[i].effect,
                facts[i].effective};
            if (facts[i].outcome == Outcome::warning)
                owner.warnings_[owner.warning_count_++] = i;
        }
        Tree tree;
        project(tree, input, facts, status);
        auto canonical = core::canonicalize_json(tree.root);
        core::detail::ExpectedErrorCleanup error_cleanup{canonical};
        if (!canonical)
            throw Reject{"invalid_input"};
        owner.canonical_ = std::move(*canonical);
        owner.digest_ = core::sha256(owner.canonical_);
        return owner;
    } catch (const Reject& rejected) {
        return std::unexpected{failure(rejected.code)};
    }
}
} // namespace epycbench::results::validation_event
