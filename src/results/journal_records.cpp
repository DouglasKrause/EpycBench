#include "epycbench/results/journal_records.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "journal_records_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <utility>
#include <vector>

namespace epycbench::results::journal {
namespace {
using Json = nlohmann::json;
constexpr std::int64_t exact_integer = 9007199254740991LL;
constexpr std::size_t maximum_depth = 64;
constexpr std::string_view schema = "epycbench.journal-record/v1";

auto failure(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.journal." + std::string{classification}},
            .message = "Journal record rejected",
            .operation = "journal_records",
            .os_error = std::nullopt,
            .context = Json::object(),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto valid_limits(Limits limits) noexcept -> bool {
    return limits.depth <= maximum_depth &&
           limits.body_bytes <= std::numeric_limits<std::size_t>::max() - 2;
}
auto category(std::string_view text) noexcept -> bool {
    constexpr std::array names{"creation",  "normalization",    "topology",     "planning",
                               "preflight", "runner_lifecycle", "placement",    "calibration",
                               "warmup",    "blocks",           "observations", "monitoring",
                               "stopping",  "analysis",         "failures",     "finalization"};
    return std::ranges::any_of(names, [text](auto name) { return text == name; });
}
auto producer(std::string_view text) noexcept -> bool {
    return text == "controller" || text == "runner" || text == "results";
}
auto digits(std::string_view text) noexcept -> bool {
    return std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}
auto number(std::string_view text) noexcept -> unsigned {
    unsigned result{};
    for (char c : text) {
        result = result * 10U + static_cast<unsigned>(c - '0');
    }
    return result;
}
auto timestamp_valid(std::string_view t) noexcept -> bool {
    if (t.size() != 30 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' ||
        t[16] != ':' || t[19] != '.' || t[29] != 'Z') {
        return false;
    }
    for (const auto& [offset, size] : std::array<std::pair<std::size_t, std::size_t>, 7>{
             {{0, 4}, {5, 2}, {8, 2}, {11, 2}, {14, 2}, {17, 2}, {20, 9}}}) {
        if (!digits(t.substr(offset, size))) {
            return false;
        }
    }
    const auto year = number(t.substr(0, 4));
    const auto month = number(t.substr(5, 2));
    const auto day = number(t.substr(8, 2));
    constexpr std::array days{31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    if (year == 0 || month == 0 || month > 12) {
        return false;
    }
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const auto max_day = days[month - 1] + (month == 2 && leap ? 1U : 0U);
    return day > 0 && day <= max_day && number(t.substr(11, 2)) < 24 &&
           number(t.substr(14, 2)) < 60 && number(t.substr(17, 2)) < 60;
}
auto digest_text(std::string_view text) noexcept -> bool {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// The admitted tree is depth-bounded. Clearing descendants first avoids the
// JSON library's allocating container destructor on both success and unwinding.
void retire(Json& value) noexcept {
    if (value.is_structured()) {
        for (auto& child : value) {
            retire(child);
        }
        value.clear();
    }
}
struct Retire {
    Json& value;
    ~Retire() { retire(value); }
};

// SAX pass rejects duplicates and excessive structure before constructing a DOM.
class Preflight final : public nlohmann::json_sax<Json> {
  public:
    explicit Preflight(Limits limits) : limits_{limits} {}
    auto null() -> bool override { return scalar(); }
    auto boolean(bool) -> bool override { return scalar(); }
    auto number_integer(number_integer_t value) -> bool override {
        return numeric(value >= -exact_integer && value <= exact_integer);
    }
    auto number_unsigned(number_unsigned_t value) -> bool override {
        return numeric(value <= static_cast<std::uint64_t>(exact_integer));
    }
    auto number_float(number_float_t, const string_t&) -> bool override {
        error = "canonicalization";
        return false;
    }
    auto string(string_t&) -> bool override { return scalar(); }
    auto binary(binary_t&) -> bool override {
        error = "canonicalization";
        return false;
    }
    auto start_object(std::size_t) -> bool override { return container(); }
    auto start_array(std::size_t) -> bool override { return container(); }
    auto key(string_t& value) -> bool override {
        auto& keys = keys_[depth_ - 1];
        if (std::ranges::find(keys, value) != keys.end()) {
            error = "envelope";
            return false;
        }
        keys.push_back(value);
        return true;
    }
    auto end_object() -> bool override { return end(); }
    auto end_array() -> bool override { return end(); }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&)
        -> bool override {
        error = "envelope";
        return false;
    }
    std::string_view error{"envelope"};

  private:
    auto scalar() -> bool {
        if (depth_ + 1 > limits_.depth || nodes_ == limits_.nodes) {
            error = "capacity";
            return false;
        }
        ++nodes_;
        return true;
    }
    auto numeric(bool valid) -> bool {
        if (!valid) {
            error = "canonicalization";
            return false;
        }
        return scalar();
    }
    auto container() -> bool {
        if (!scalar()) {
            return false;
        }
        ++depth_;
        return true;
    }
    auto end() -> bool {
        keys_[--depth_].clear();
        return true;
    }
    Limits limits_;
    std::size_t depth_{};
    std::size_t nodes_{};
    std::array<std::vector<std::string>, maximum_depth> keys_;
};

// Explicit bounded traversal validates caller-owned payload before any deep copy.
auto payload_size(const Json& payload, Limits limits) -> std::expected<std::size_t, core::Error> {
    struct Frame {
        const Json* value;
        Json::const_iterator next;
        bool entered;
    };
    std::array<Frame, maximum_depth> stack{};
    std::size_t used = 1;
    stack[0] = {&payload, {}, false};
    std::size_t nodes = 10; // Root plus nine non-payload envelope values.
    std::size_t bytes{};
    auto add = [&](std::size_t n) {
        if (n > limits.body_bytes - bytes) {
            return false;
        }
        bytes += n;
        return true;
    };
    auto string_size = [&](std::string_view s) {
        if (!add(2)) {
            return false;
        }
        for (char byte : s) {
            const auto c = static_cast<unsigned char>(byte);
            const std::size_t n = c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' ||
                                          c == '\r' || c == '\t'
                                      ? 2U
                                  : c < 0x20 ? 6U
                                             : 1U;
            if (!add(n)) {
                return false;
            }
        }
        return true;
    };
    while (used != 0) {
        auto& frame = stack[used - 1];
        const auto& value = *frame.value;
        if (!frame.entered) {
            if (used + 1 > limits.depth || nodes >= limits.nodes) {
                return std::unexpected{failure("capacity")};
            }
            ++nodes;
            frame.entered = true;
            if (value.is_object() || value.is_array()) {
                if (!add(2)) {
                    return std::unexpected{failure("capacity")};
                }
                frame.next = value.begin();
            } else {
                bool fits{};
                if (value.is_string()) {
                    fits = string_size(value.get_ref<const std::string&>());
                } else if (value.is_null()) {
                    fits = add(4);
                } else if (value.is_boolean()) {
                    fits = add(value.get<bool>() ? 4U : 5U);
                } else if (value.is_number_integer()) {
                    std::array<char, 32> buffer{};
                    std::to_chars_result converted{};
                    if (value.is_number_unsigned()) {
                        const auto v = value.get<std::uint64_t>();
                        if (v > static_cast<std::uint64_t>(exact_integer)) {
                            return std::unexpected{failure("canonicalization")};
                        }
                        converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
                    } else {
                        const auto v = value.get<std::int64_t>();
                        if (v < -exact_integer || v > exact_integer) {
                            return std::unexpected{failure("canonicalization")};
                        }
                        converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v);
                    }
                    fits = add(static_cast<std::size_t>(converted.ptr - buffer.data()));
                } else {
                    return std::unexpected{failure("canonicalization")};
                }
                if (!fits) {
                    return std::unexpected{failure("capacity")};
                }
                --used;
                continue;
            }
        }
        if (frame.next == value.end()) {
            --used;
            continue;
        }
        if (frame.next != value.begin() && !add(1)) {
            return std::unexpected{failure("capacity")};
        }
        if (value.is_object() && (!string_size(frame.next.key()) || !add(1))) {
            return std::unexpected{failure("capacity")};
        }
        if (used == stack.size()) {
            return std::unexpected{failure("capacity")};
        }
        const auto* child = &*frame.next++;
        stack[used++] = {child, {}, false};
    }
    return bytes;
}

auto decode(std::string_view body, const RunId& run, Limits limits, std::uint64_t sequence,
            const std::optional<core::Sha256Digest>& previous)
    -> std::expected<Record, core::Error> {
    Preflight preflight{limits};
    if (!Json::sax_parse(body, &preflight)) {
        return std::unexpected{failure(preflight.error)};
    }
    auto value = Json::parse(body);
    Retire cleanup{value};
    auto canonical = core::canonicalize_json(value);
    if (!canonical || *canonical != body) {
        return std::unexpected{failure("canonicalization")};
    }
    constexpr std::array fields{"schema",    "run_id",   "sequence",        "event_id", "type",
                                "timestamp", "producer", "previous_digest", "payload",  "digest"};
    if (!value.is_object() || value.size() != fields.size()) {
        return std::unexpected{failure("envelope")};
    }
    for (auto field : fields) {
        if (!value.contains(field)) {
            return std::unexpected{failure("envelope")};
        }
        if (std::string_view{field} != "payload" && std::string_view{field} != "previous_digest" &&
            !value[field].is_string()) {
            return std::unexpected{failure("envelope")};
        }
    }
    auto text = [&](const char* key) -> const std::string& {
        return value[key].get_ref<const std::string&>();
    };
    if (text("schema") != schema || !category(text("type")) || !producer(text("producer")) ||
        !value["payload"].is_object() || !digest_text(text("digest"))) {
        return std::unexpected{failure("envelope")};
    }
    if (!timestamp_valid(text("timestamp"))) {
        return std::unexpected{failure("timestamp")};
    }
    if (text("run_id") != run.text()) {
        return std::unexpected{failure("identity")};
    }
    const auto seq = std::to_string(sequence);
    if (text("sequence") != seq || text("event_id") != run.text() + ":" + seq) {
        return std::unexpected{failure("sequence")};
    }
    if (previous ? (!value["previous_digest"].is_string() ||
                    text("previous_digest") != previous->to_hex())
                 : !value["previous_digest"].is_null()) {
        return std::unexpected{failure("chain")};
    }
    const auto digest = text("digest");
    value.erase("digest");
    auto hash = core::hash_canonical_json(value);
    if (!hash || hash->to_hex() != digest) {
        return std::unexpected{failure("digest")};
    }
    value["digest"] = digest;
    return Record{std::move(value), std::move(*canonical)};
}
} // namespace

auto RunId::parse(std::string_view text) -> std::expected<RunId, core::Error> {
    if (text.size() != 36) {
        return std::unexpected{failure("identity")};
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return std::unexpected{failure("identity")};
            }
        } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) {
            return std::unexpected{failure("identity")};
        }
    }
    if (text[14] != '7' ||
        (text[19] != '8' && text[19] != '9' && text[19] != 'a' && text[19] != 'b')) {
        return std::unexpected{failure("identity")};
    }
    return RunId{std::string{text}};
}
auto Timestamp::parse(std::string_view text) -> std::expected<Timestamp, core::Error> {
    if (!timestamp_valid(text)) {
        return std::unexpected{failure("timestamp")};
    }
    return Timestamp{std::string{text}};
}
Writer::Writer(RunId run, Limits limits) : run_{std::move(run)}, limits_{limits} {}
Reader::Reader(RunId run, Limits limits) : run_{std::move(run)}, limits_{limits} {}
Writer::Writer(Writer&& other) noexcept
    : run_{std::move(other.run_)}, limits_{other.limits_}, count_{other.count_},
      previous_{other.previous_}, live_{std::exchange(other.live_, false)} {}
Reader::Reader(Reader&& other) noexcept
    : run_{std::move(other.run_)}, limits_{other.limits_}, count_{other.count_},
      previous_{other.previous_}, body_{std::move(other.body_)}, in_record_{other.in_record_},
      live_{std::exchange(other.live_, false)} {}
auto Writer::create(RunId run, Limits limits) -> std::expected<Writer, core::Error> {
    if (!valid_limits(limits)) {
        return std::unexpected{failure("capacity")};
    }
    if (!RunId::parse(run.text())) {
        return std::unexpected{failure("identity")};
    }
    return Writer{std::move(run), limits};
}
auto Reader::create(RunId run, Limits limits) -> std::expected<Reader, core::Error> {
    if (!valid_limits(limits)) {
        return std::unexpected{failure("capacity")};
    }
    if (!RunId::parse(run.text())) {
        return std::unexpected{failure("identity")};
    }
    return Reader{std::move(run), limits};
}
auto Writer::append(const Event& event) -> std::expected<std::string, core::Error> {
    if (!std::exchange(live_, false)) {
        return std::unexpected{failure("lifecycle")};
    }
    const auto next = detail::next_sequence(count_);
    if (!next || count_ == limits_.records) {
        return std::unexpected{failure("capacity")};
    }
    if (!category(event.type) || !producer(event.producer) || !event.payload.is_object()) {
        return std::unexpected{failure("envelope")};
    }
    // Moved-from validated scalar wrappers cannot bypass validation.
    if (!RunId::parse(run_.text())) {
        return std::unexpected{failure("identity")};
    }
    if (!timestamp_valid(event.timestamp.text())) {
        return std::unexpected{failure("timestamp")};
    }
    const auto size = payload_size(event.payload, limits_);
    if (!size) {
        return std::unexpected{size.error()};
    }
    const auto seq = std::to_string(*next);
    Json value = Json::object();
    Retire cleanup{value};
    value["schema"] = schema;
    value["run_id"] = run_.text();
    value["sequence"] = seq;
    value["event_id"] = run_.text() + ":" + seq;
    value["type"] = event.type;
    value["timestamp"] = event.timestamp.text();
    value["producer"] = event.producer;
    value["payload"] = nullptr;
    value["digest"] = std::string(64, '0');
    value["previous_digest"] = previous_ ? Json(previous_->to_hex()) : Json(nullptr);
    auto skeleton = core::canonicalize_json(value);
    if (!skeleton || skeleton->size() < 4 || skeleton->size() - 4 > limits_.body_bytes ||
        *size > limits_.body_bytes - (skeleton->size() - 4)) {
        return std::unexpected{failure("capacity")};
    }
    value["payload"] = event.payload;
    value.erase("digest");
    auto digest = core::hash_canonical_json(value);
    if (!digest) {
        return std::unexpected{failure("canonicalization")};
    }
    value["digest"] = digest->to_hex();
    auto body = core::canonicalize_json(value);
    if (!body) {
        return std::unexpected{failure("canonicalization")};
    }
    std::string frame;
    frame.reserve(body->size() + 2);
    frame.push_back('\x1e');
    frame += *body;
    frame.push_back('\n');
    count_ = *next;
    previous_ = *digest;
    live_ = true;
    return frame;
}
auto Writer::finish() -> std::expected<Summary, core::Error> {
    if (!std::exchange(live_, false)) {
        return std::unexpected{failure("lifecycle")};
    }
    return Summary{Tail::clean_prefix, count_, previous_, 0};
}
auto Reader::feed(std::string_view bytes) -> std::expected<Feed, core::Error> {
    if (!std::exchange(live_, false)) {
        return std::unexpected{failure("lifecycle")};
    }
    std::size_t consumed{};
    for (char byte : bytes) {
        ++consumed;
        if (!in_record_) {
            if (byte != '\x1e') {
                return std::unexpected{failure("framing")};
            }
            if (count_ == limits_.records || !detail::next_sequence(count_)) {
                return std::unexpected{failure("capacity")};
            }
            in_record_ = true;
        } else if (byte == '\x1e') {
            return std::unexpected{failure("framing")};
        } else if (byte == '\n') {
            auto record = decode(body_, run_, limits_, *detail::next_sequence(count_), previous_);
            if (!record) {
                return std::unexpected{record.error()};
            }
            auto digest =
                core::Sha256Digest::parse(record->envelope["digest"].get_ref<const std::string&>());
            if (!digest) {
                return std::unexpected{failure("digest")};
            }
            Feed result{consumed, std::move(*record)};
            ++count_;
            previous_ = *digest;
            body_.clear();
            in_record_ = false;
            live_ = true;
            return result;
        } else {
            if (body_.size() == limits_.body_bytes) {
                return std::unexpected{failure("capacity")};
            }
            body_.push_back(byte);
        }
    }
    live_ = true;
    return Feed{consumed, std::nullopt};
}
auto Reader::finish() -> std::expected<Summary, core::Error> {
    if (!std::exchange(live_, false)) {
        return std::unexpected{failure("lifecycle")};
    }
    return Summary{in_record_ ? Tail::incomplete_tail : Tail::clean_prefix, count_, previous_,
                   in_record_ ? body_.size() + 1 : 0};
}
} // namespace epycbench::results::journal
