#include "experiment/ledger_store.hpp"

#include "epycbench/core/canonical_json.hpp"
#include "platform_linux/experiment_operations.hpp"

#include <array>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace epycbench::experiment {
namespace {

using Json = nlohmann::json;
using platform_linux::experiment_operations::Directory;
using namespace std::literals::string_view_literals;

constexpr auto profile = "epycbench.experiment-event/v1"sv;
constexpr auto event_prefix = "event-"sv;
constexpr std::size_t event_bytes = 4096;
constexpr std::size_t sequence_digits = 8;
constexpr int maximum_exit_code = 255;
constexpr int maximum_signal = 127;

constexpr std::array dispositions{
    std::pair{Disposition::completed, "completed"sv},
    std::pair{Disposition::invalid, "invalid"sv},
    std::pair{Disposition::failed, "failed"sv},
    std::pair{Disposition::interrupted, "interrupted"sv},
};
constexpr std::array unavailable_reasons{
    std::pair{UnavailableResult::run_failed, "run_failed"sv},
    std::pair{UnavailableResult::invalid_result, "invalid_result"sv},
    std::pair{UnavailableResult::result_missing, "result_missing"sv},
    std::pair{UnavailableResult::interrupted, "interrupted"sv},
};
constexpr std::array stop_reasons{
    std::pair{StopReason::scientific, "scientific"sv},
    std::pair{StopReason::user_interrupted, "user_interrupted"sv},
};

auto failure(std::string_view classification, std::uint64_t sequence = 0)
    -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{"experiment.ledger_store.invalid"},
        .message = "Experiment ledger could not be validated",
        .operation = "experiment.ledger_store",
        .os_error = {},
        .context = {{"classification", classification}, {"sequence", sequence}},
    }};
}

auto filename(std::uint64_t sequence) -> std::string {
    const auto digits = std::to_string(sequence);
    return std::string{event_prefix} + std::string(sequence_digits - digits.size(), '0') + digits +
           ".json";
}

bool valid_association(const ResultAssociation& result) noexcept {
    constexpr auto identifier_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"sv;
    constexpr auto filename_chars =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:"sv;
    return !result.run_id.empty() && result.run_id.size() <= 64 &&
           result.run_id.find_first_not_of(identifier_chars) == std::string::npos &&
           !result.artifact.empty() && result.artifact.size() <= 255 &&
           result.artifact != "." && result.artifact != ".." &&
           result.artifact.find_first_not_of(filename_chars) == std::string::npos;
}

struct MalformedEvent {};

auto encode_event(const Event& event) -> Json {
    if (const auto* reservation = std::get_if<Reservation>(&event)) {
        return {{"kind", "reservation"}, {"trial_sha256", reservation->trial.to_hex()}};
    }
    if (const auto* outcome = std::get_if<Outcome>(&event)) {
        if ((outcome->result && !valid_association(*outcome->result)) ||
            (outcome->exit_code && (*outcome->exit_code < 0 || *outcome->exit_code > maximum_exit_code)) ||
            (outcome->signal && (*outcome->signal < 1 || *outcome->signal > maximum_signal))) {
            throw MalformedEvent{};
        }
        Json result = nullptr;
        if (outcome->result) {
            result = {{"run_id", outcome->result->run_id},
                      {"content_sha256", outcome->result->content_digest.to_hex()},
                      {"artifact", outcome->result->artifact}};
        }
        return {{"kind", "outcome"},
                {"trial_sha256", outcome->trial.to_hex()},
                {"disposition", core::detail::enum_name(outcome->disposition, dispositions)},
                {"result", std::move(result)},
                {"unavailable", outcome->unavailable
                    ? Json(core::detail::enum_name(*outcome->unavailable, unavailable_reasons))
                    : Json(nullptr)},
                {"exit_code", outcome->exit_code ? Json(*outcome->exit_code) : Json(nullptr)},
                {"signal", outcome->signal ? Json(*outcome->signal) : Json(nullptr)},
                {"scientific_stop", outcome->scientific_stop}};
    }
    return {{"kind", "stop"},
            {"reason", core::detail::enum_name(std::get<Stop>(event).reason, stop_reasons)}};
}

auto string_value(const Json& value) -> const std::string& {
    return value.get_ref<const std::string&>();
}

auto digest_value(const Json& value) -> core::Sha256Digest {
    const auto parsed = core::Sha256Digest::parse(string_value(value));
    if (!parsed) throw MalformedEvent{};
    return *parsed;
}

auto process_status(const Json& value, std::uint64_t lowest, std::uint64_t highest)
    -> std::optional<int> {
    if (value.is_null()) return {};
    if (!value.is_number_unsigned()) throw MalformedEvent{};
    const auto status = value.get<std::uint64_t>();
    if (status < lowest || status > highest) throw MalformedEvent{};
    return static_cast<int>(status);
}

auto decode_event(const Json& event) -> Event {
    const auto& kind = string_value(event.at("kind"));
    if (kind == "reservation") return Reservation{digest_value(event.at("trial_sha256"))};
    if (kind == "stop") {
        return Stop{core::detail::parse_enum(string_value(event.at("reason")), stop_reasons)};
    }
    if (kind != "outcome") throw MalformedEvent{};
    std::optional<ResultAssociation> result;
    const auto& association = event.at("result");
    if (!association.is_null()) {
        result.emplace(string_value(association.at("run_id")),
                       digest_value(association.at("content_sha256")),
                       string_value(association.at("artifact")));
    }
    std::optional<UnavailableResult> unavailable;
    const auto& unavailable_reason = event.at("unavailable");
    if (!unavailable_reason.is_null()) {
        unavailable = core::detail::parse_enum(string_value(unavailable_reason), unavailable_reasons);
    }
    return Outcome{
        digest_value(event.at("trial_sha256")),
        core::detail::parse_enum(string_value(event.at("disposition")), dispositions),
        std::move(result),
        unavailable,
        process_status(event.at("exit_code"), 0, maximum_exit_code),
        process_status(event.at("signal"), 1, maximum_signal),
        event.at("scientific_stop").get<bool>(),
    };
}

struct EncodedEntry {
    std::string bytes;
    core::Sha256Digest digest;
};

auto encode(const Entry& entry, std::string_view plan_digest,
            const std::optional<core::Sha256Digest>& previous)
    -> std::expected<EncodedEntry, core::Error> {
    try {
        Json value{{"profile", profile},
                   {"plan_sha256", plan_digest},
                   {"sequence", entry.sequence},
                   {"previous_event_sha256", previous ? Json(previous->to_hex()) : Json(nullptr)},
                   {"event", encode_event(entry.event)}};
        auto digest = core::hash_canonical_json(value);
        if (!digest) return std::unexpected{std::move(digest.error())};
        value["event_sha256"] = digest->to_hex();
        auto bytes = core::canonicalize_json(value);
        if (!bytes) return std::unexpected{std::move(bytes.error())};
        return EncodedEntry{std::move(*bytes), *digest};
    } catch (const MalformedEvent&) {
        return failure("event_values", entry.sequence);
    } catch (const std::invalid_argument&) {
        return failure("event_enum", entry.sequence);
    }
}

struct DecodedEntry {
    Entry entry;
    core::Sha256Digest digest;
};

auto decode(std::string_view bytes, std::uint64_t sequence, std::string_view plan_digest,
            const std::optional<core::Sha256Digest>& previous)
    -> std::expected<DecodedEntry, core::Error> {
    try {
        const auto bounded_depth = [](int depth, Json::parse_event_t, Json&) {
            if (depth > 4) throw MalformedEvent{};
            return true;
        };
        const auto object = Json::parse(bytes, bounded_depth);
        Entry entry{sequence, decode_event(object.at("event"))};
        auto encoded = encode(entry, plan_digest, previous);
        if (!encoded) return std::unexpected{std::move(encoded.error())};
        // Canonical encoding of the typed entry closes the schema, checks the
        // plan and both hashes, and rejects duplicate keys and implicit casts.
        if (encoded->bytes != bytes) return failure("event_encoding_or_binding", sequence);
        return DecodedEntry{std::move(entry), encoded->digest};
    } catch (const Json::exception&) {
        return failure("event_shape", sequence);
    } catch (const MalformedEvent&) {
        return failure("event_values", sequence);
    } catch (const std::invalid_argument&) {
        return failure("event_enum", sequence);
    }
}

} // namespace

LedgerStore::LedgerStore(const Directory& directory, std::string plan_digest,
                         std::size_t maximum_entries) noexcept
    : directory_{&directory}, plan_digest_{std::move(plan_digest)}, maximum_entries_{maximum_entries} {}

LedgerStore::LedgerStore(LedgerStore&& other) noexcept
    : directory_{std::exchange(other.directory_, nullptr)}, plan_digest_{std::move(other.plan_digest_)},
      maximum_entries_{other.maximum_entries_}, next_sequence_{other.next_sequence_},
      previous_{other.previous_} {}

auto LedgerStore::operator=(LedgerStore&& other) noexcept -> LedgerStore& {
    if (this != &other) {
        directory_ = std::exchange(other.directory_, nullptr);
        plan_digest_ = std::move(other.plan_digest_);
        maximum_entries_ = other.maximum_entries_;
        next_sequence_ = other.next_sequence_;
        previous_ = other.previous_;
    }
    return *this;
}

auto LedgerStore::open(const Directory& directory, std::string plan_digest, Ledger& ledger)
    -> std::expected<LedgerStore, core::Error> {
    if (!ledger.entries().empty() || !core::Sha256Digest::parse(plan_digest)) {
        return failure("replay_admission");
    }
    const auto trials = ledger.schedule().trials().size();
    // Each trial can charge, finish and be followed by a user stop. The initial
    // user stop and a terminal scientific stop are the only additional events.
    LedgerStore store{directory, std::move(plan_digest), 3 * trials + 2};
    auto names = directory.list(store.maximum_entries_ + trials + 16);
    if (!names) return std::unexpected{std::move(names.error())};
    Ledger replay{ledger.schedule()};
    for (const auto& name : *names) {
        if (!name.starts_with(event_prefix)) continue;
        const auto sequence = store.next_sequence_;
        if (sequence > store.maximum_entries_ || name != filename(sequence)) {
            return failure("event_filename_or_sequence", sequence);
        }
        auto bytes = directory.read(name, event_bytes);
        if (!bytes) return std::unexpected{std::move(bytes.error())};
        auto decoded = decode(*bytes, sequence, store.plan_digest_, store.previous_);
        if (!decoded) return std::unexpected{std::move(decoded.error())};
        auto matched = replay.append(std::move(decoded->entry));
        if (!matched) return std::unexpected{std::move(matched.error())};
        store.previous_ = decoded->digest;
        ++store.next_sequence_;
    }
    ledger = std::move(replay);
    return store;
}

auto LedgerStore::persist(const Entry& entry) -> std::expected<void, core::Error> {
    if (!directory_ || entry.sequence != next_sequence_ || next_sequence_ > maximum_entries_) {
        return failure("persist_admission", entry.sequence);
    }
    auto encoded = encode(entry, plan_digest_, previous_);
    if (!encoded) return std::unexpected{std::move(encoded.error())};
    auto written = directory_->write_new(filename(entry.sequence), encoded->bytes, event_bytes);
    if (!written) {
        directory_ = nullptr;
        return written;
    }
    previous_ = encoded->digest;
    ++next_sequence_;
    return {};
}

} // namespace epycbench::experiment
