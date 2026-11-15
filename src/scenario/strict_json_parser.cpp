#include "scenario/strict_json_parser.hpp"
#include "core/json_ownership.hpp"
#include "scenario/json_cleanup.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::scenario::detail {
namespace {

constexpr std::size_t max_depth = 64;
constexpr std::size_t max_mapping = 65'536;
constexpr std::size_t max_sequence = 262'144;
constexpr std::size_t max_nodes = 262'144;
constexpr std::size_t max_scalar = 1U << 20U;
constexpr std::size_t max_key = 4'096;

auto error(std::string code, std::string classification) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["format"] = "json";
    context["classification"] = std::move(classification);
    return core::Error{.category = core::ErrorCategory::scenario,
                       .code = core::StableErrorCode{std::move(code)},
                       .message = "Authored JSON violates the strict parser boundary",
                       .operation = "parse_authored_scenario",
                       .os_error = std::nullopt,
                       .context = std::move(context)};
}

auto integer_lexeme(std::string_view token) -> bool {
    if (!token.empty() && token.front() == '-') token.remove_prefix(1);
    if (token.empty()) return false;
    for (const char value : token) {
        if (value < '0' || value > '9') return false;
    }
    return true;
}

struct Frame {
    bool object{};
    nlohmann::ordered_json value;
    std::string key;
    std::map<std::string, bool> keys;
    std::size_t entries{};
};

class Sax final : public nlohmann::json_sax<nlohmann::ordered_json> {
  public:
    ~Sax() {
        for (auto& frame : frames_)
            retire_json(frame.value);
        if (root_)
            retire_json(*root_);
        if (error_)
            retire_json(error_->context);
    }
    bool null() override { return scalar(nullptr); }
    bool boolean(bool value) override { return scalar(value); }
    bool number_integer(number_integer_t value) override { return scalar(value); }
    bool number_unsigned(number_unsigned_t value) override { return scalar(value); }
    bool number_float(number_float_t value, const string_t& token) override {
        if (integer_lexeme(token)) return fail("scenario.parse.integer_out_of_range", "integer_range");
        if (!std::isfinite(value)) return fail("scenario.parse.nonfinite_number", "floating_point");
        return scalar(value);
    }
    bool string(string_t& value) override {
        if (value.size() > max_scalar) return fail("scenario.parse.scalar_limit", "scalar_bytes");
        return scalar(std::move(value));
    }
    bool binary(binary_t&) override { return fail("scenario.parse.invalid_scalar", "binary"); }

    bool start_object(std::size_t) override { return start(true); }
    bool key(string_t& value) override {
        if (frames_.empty() || !frames_.back().object) return fail("scenario.parse.syntax_error", "key_state");
        if (value.size() > max_key) return fail("scenario.parse.key_limit", "mapping_key_bytes");
        auto& frame = frames_.back();
        if (frame.keys.contains(value)) return fail("scenario.parse.duplicate_key", "duplicate_key");
        frame.keys.emplace(value, true);
        frame.key = std::move(value);
        return true;
    }
    bool end_object() override { return finish(); }
    bool start_array(std::size_t) override { return start(false); }
    bool end_array() override { return finish(); }

    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& exception) override {
        if (!error_) {
            if (root_ && frames_.empty()) error_ = error("scenario.parse.trailing_content", "trailing_content");
            else error_ = exception.id == 406 ? error("scenario.parse.nonfinite_number", "floating_point")
                                              : error("scenario.parse.syntax_error", "json_syntax");
            error_->context["byte_offset"] = position == 0 ? 0 : position - 1;
        }
        return false;
    }

    auto result() -> std::expected<nlohmann::ordered_json, core::Error> {
        if (error_) return std::unexpected{std::move(*error_)};
        if (!root_) return std::unexpected{error("scenario.parse.empty_document", "empty_document")};
        if (!frames_.empty()) return std::unexpected{error("scenario.parse.syntax_error", "unclosed_collection")};
        return std::move(*root_);
    }

  private:
    bool fail(std::string code, std::string classification) {
        if (!error_) error_ = error(std::move(code), std::move(classification));
        return false;
    }

    bool count_node() {
        if (frames_.empty() && !root_) return true;
        if (nodes_ >= max_nodes) return fail("scenario.parse.node_limit", "total_nodes");
        ++nodes_;
        return true;
    }

    bool start(bool object) {
        if (frames_.size() >= max_depth) return fail("scenario.parse.depth_limit", "collection_depth");
        if (!can_attach()) return false;
        if (!count_node()) return false;
        frames_.push_back(Frame{.object = object,
                                .value = object ? nlohmann::ordered_json::object()
                                                : nlohmann::ordered_json::array(),
                                .key = {},
                                .keys = {},
                                .entries = 0});
        return true;
    }

    bool finish() {
        if (frames_.empty()) return fail("scenario.parse.syntax_error", "unexpected_collection_end");
        auto value = std::move(frames_.back().value);
        JsonCleanup cleanup{value};
        frames_.pop_back();
        return attach(std::move(value));
    }

    template <typename Value> bool scalar(Value&& value) {
        if (!can_attach()) return false;
        if (!count_node()) return false;
        return attach(nlohmann::ordered_json(std::forward<Value>(value)));
    }

    bool can_attach() {
        if (frames_.empty()) return true;
        const auto& frame = frames_.back();
        if (frame.object && frame.entries >= max_mapping)
            return fail("scenario.parse.mapping_limit", "mapping_entries");
        if (!frame.object && frame.entries >= max_sequence)
            return fail("scenario.parse.sequence_limit", "sequence_entries");
        return true;
    }

    bool attach(nlohmann::ordered_json value) {
        JsonCleanup cleanup{value};
        if (frames_.empty()) {
            if (root_) return fail("scenario.parse.trailing_content", "second_root");
            root_ = std::move(value);
            return true;
        }
        auto& frame = frames_.back();
        if (frame.object) {
            if (frame.entries >= max_mapping) return fail("scenario.parse.mapping_limit", "mapping_entries");
            append_object(frame.value, frame.key, value);
            frame.key.clear();
        } else {
            if (frame.entries >= max_sequence) return fail("scenario.parse.sequence_limit", "sequence_entries");
            frame.value.push_back(std::move(value));
        }
        ++frame.entries;
        return true;
    }

    std::vector<Frame> frames_;
    std::optional<nlohmann::ordered_json> root_;
    std::optional<core::Error> error_;
    std::size_t nodes_{};
};

} // namespace

auto parse_strict_json(std::string_view contents)
    -> std::expected<nlohmann::ordered_json, core::Error> {
    Sax sax;
    const bool accepted = nlohmann::ordered_json::sax_parse(contents.begin(), contents.end(), &sax,
                                                             nlohmann::json::input_format_t::json, true,
                                                             false);
    auto result = sax.result();
    if (!accepted && result) return std::unexpected{error("scenario.parse.syntax_error", "json_syntax")};
    return result;
}

auto verify_json_location_value(const nlohmann::ordered_json& strict,
                                const nlohmann::ordered_json& marked)
    -> std::expected<void, core::Error> {
    if (strict != marked) {
        return std::unexpected{error("scenario.parse.internal_parser_failure", "json_location_mismatch")};
    }
    return {};
}

} // namespace epycbench::scenario::detail
