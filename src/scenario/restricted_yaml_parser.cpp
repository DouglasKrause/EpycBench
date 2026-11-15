#include "scenario/restricted_yaml_parser.hpp"
#include "core/json_ownership.hpp"

#include "scenario/json_cleanup.hpp"
#include "scenario/scalar_resolver.hpp"

#include <libfyaml.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
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

auto make_error(std::string code, std::string classification,
                std::optional<SourceLocation> location = std::nullopt,
                std::string pointer = {}) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};
    context["format"] = "yaml";
    context["classification"] = std::move(classification);
    if (location) {
        context["byte_offset"] = location->byte_offset;
        context["line"] = location->line;
        context["column"] = location->column;
    }
    if (!pointer.empty()) context["parent_pointer"] = std::move(pointer);
    return core::Error{.category = core::ErrorCategory::scenario,
                       .code = core::StableErrorCode{std::move(code)},
                       .message = "Authored YAML violates the restricted parser boundary",
                       .operation = "parse_authored_scenario",
                       .os_error = std::nullopt,
                       .context = std::move(context)};
}

auto location(const fy_mark* mark) -> SourceLocation {
    if (mark == nullptr) return {};
    return {.byte_offset = static_cast<std::uint64_t>(mark->input_pos),
            .line = static_cast<std::uint64_t>(mark->line) + 1U,
            .column = static_cast<std::uint64_t>(mark->column) + 1U};
}

auto escape_pointer(std::string_view text) -> std::string {
    std::string result;
    for (const char value : text) {
        if (value == '~') result += "~0";
        else if (value == '/') result += "~1";
        else result.push_back(value);
    }
    return result;
}

struct ParserDeleter {
    void operator()(fy_parser* parser) const noexcept { fy_parser_destroy(parser); }
};
using ParserPtr = std::unique_ptr<fy_parser, ParserDeleter>;
struct DiagDeleter {
    void operator()(fy_diag* diag) const noexcept { fy_diag_destroy(diag); }
};
using DiagPtr = std::unique_ptr<fy_diag, DiagDeleter>;

void discard_diagnostic(fy_diag*, void*, const char*, std::size_t) {}

auto make_diag() -> DiagPtr {
    fy_diag_cfg cfg{};
    fy_diag_cfg_default(&cfg);
    cfg.fp = nullptr;
    cfg.output_fn = discard_diagnostic;
    cfg.user = nullptr;
    cfg.colorize = false;
    cfg.show_source = false;
    cfg.show_position = false;
    DiagPtr result{fy_diag_create(&cfg)};
    if (result) fy_diag_set_collect_errors(result.get(), true);
    return result;
}

struct EventGuard {
    fy_parser* parser{};
    fy_event* event{};
    ~EventGuard() { fy_parser_event_free(parser, event); }
};

auto config(bool json, fy_diag* diag) -> fy_parse_cfg {
    fy_parse_cfg result{};
    result.search_path = nullptr;
    result.diag = diag;
    result.flags = static_cast<fy_parse_cfg_flags>(
        FYPCF_QUIET | FYPCF_COLLECT_DIAG | FYPCF_DEFAULT_VERSION_1_2 |
        (json ? FYPCF_JSON_FORCE : FYPCF_JSON_NONE) | FYPCF_DISABLE_MMAP_OPT |
        FYPCF_DISABLE_RECYCLING | FYPCF_DISABLE_ACCELERATORS |
        FYPCF_DISABLE_BUFFERING | FYPCF_ALLOW_DUPLICATE_KEYS);
    return result;
}

auto preflight_explicit_keys(std::string_view input) -> std::expected<void, core::Error> {
    auto diag = make_diag();
    if (!diag) return std::unexpected{make_error("scenario.parse.internal_parser_failure", "diagnostic_setup")};
    auto cfg = config(false, diag.get());
    ParserPtr parser{fy_parser_create(&cfg)};
    if (!parser || fy_parser_set_string(parser.get(), input.data(), input.size()) != 0) {
        return std::unexpected{make_error("scenario.parse.internal_parser_failure", "scanner_setup")};
    }
    while (fy_token* token = fy_scan(parser.get())) {
        const auto type = fy_token_get_type(token);
        const auto start = fy_token_start_mark(token);
        const auto end = fy_token_end_mark(token);
        const auto where = location(start);
        const bool explicit_key = type == FYTT_KEY && start != nullptr && end != nullptr &&
                                  end->input_pos > start->input_pos;
        fy_scan_token_free(parser.get(), token);
        if (explicit_key) {
            return std::unexpected{make_error("scenario.parse.complex_key_not_allowed",
                                              "explicit_key_syntax", where)};
        }
    }
    if (fy_parser_get_stream_error(parser.get())) {
        return std::unexpected{make_error("scenario.parse.syntax_error", "scanner_error")};
    }
    return {};
}

struct Frame {
    bool mapping{};
    nlohmann::ordered_json value;
    std::string pointer;
    SourceLocation start;
    std::optional<SourceLocation> own_key;
    bool expecting_key{};
    std::string key;
    SourceLocation key_location;
    std::map<std::string, SourceLocation> keys;
    std::size_t entries{};
};

class Builder {
  public:
    ~Builder() {
        for (auto& frame : frames_)
            retire_json(frame.value);
        if (root_)
            retire_json(*root_);
    }
    auto consume(fy_event* event) -> std::expected<void, core::Error> {
        const auto type = fy_event_get_type(event);
        if (type == FYET_DOCUMENT_START) return document_start(event);
        if (type == FYET_DOCUMENT_END) return document_end();
        if (type == FYET_ALIAS) return fail("scenario.parse.alias_not_allowed", "alias", event);
        if (type == FYET_SCALAR || type == FYET_MAPPING_START || type == FYET_SEQUENCE_START) {
            if (fy_event_get_tag_token(event) != nullptr)
                return fail("scenario.parse.tag_not_allowed", "explicit_tag", event);
            if (fy_event_get_anchor_token(event) != nullptr)
                return fail("scenario.parse.anchor_not_allowed", "anchor", event);
        }
        if (type == FYET_SCALAR) return scalar(event);
        if (type == FYET_MAPPING_START) return begin(event, true);
        if (type == FYET_SEQUENCE_START) return begin(event, false);
        if (type == FYET_MAPPING_END || type == FYET_SEQUENCE_END) return end_collection();
        return {};
    }

    auto finish(bool stream_error) -> std::expected<ParsedDocument, core::Error> {
        if (stream_error) return std::unexpected{make_error("scenario.parse.syntax_error", "backend_syntax")};
        if (documents_ == 0 || !root_) {
            return std::unexpected{make_error("scenario.parse.empty_document", "empty_document")};
        }
        if (documents_ > 1) {
            return std::unexpected{make_error("scenario.parse.multiple_documents", "multiple_documents")};
        }
        if (!frames_.empty()) {
            return std::unexpected{make_error("scenario.parse.syntax_error", "unclosed_collection")};
        }
        return ParsedDocument{.value = std::move(*root_), .locations = std::move(locations_)};
    }

  private:
    auto fail(std::string code, std::string classification, fy_event* event)
        -> std::expected<void, core::Error> {
        return std::unexpected{make_error(std::move(code), std::move(classification),
                                          location(fy_event_style_start_mark(event)), current_pointer())};
    }

    auto document_start(fy_event* event) -> std::expected<void, core::Error> {
        ++documents_;
        if (documents_ > 1) return fail("scenario.parse.multiple_documents", "multiple_documents", event);
        auto* state = event->document_start.document_state;
        if (state != nullptr && (fy_document_state_version_explicit(state) ||
                                 fy_document_state_tags_explicit(state))) {
            return fail("scenario.parse.directive_not_allowed", "directive", event);
        }
        return {};
    }

    auto document_end() -> std::expected<void, core::Error> { return {}; }

    auto begin(fy_event* event, bool mapping) -> std::expected<void, core::Error> {
        if (is_mapping_key())
            return fail("scenario.parse.complex_key_not_allowed", "complex_key", event);
        if (frames_.size() >= max_depth)
            return fail("scenario.parse.depth_limit", "collection_depth", event);
        auto attachment = next_attachment();
        if (!attachment)
            return std::unexpected{std::move(attachment.error())};
        auto counted = count_node(event);
        if (!counted)
            return std::unexpected{std::move(counted.error())};
        frames_.push_back(Frame{.mapping = mapping,
                                .value = mapping ? nlohmann::ordered_json::object()
                                                 : nlohmann::ordered_json::array(),
                                .pointer = attachment->first,
                                .start = location(fy_event_style_start_mark(event)),
                                .own_key = attachment->second,
                                .expecting_key = mapping,
                                .key = {},
                                .key_location = {},
                                .keys = {},
                                .entries = 0});
        return {};
    }

    auto end_collection() -> std::expected<void, core::Error> {
        if (frames_.empty()) return std::unexpected{make_error("scenario.parse.syntax_error", "unexpected_end")};
        Frame completed = std::move(frames_.back());
        JsonCleanup completed_cleanup{completed.value};
        frames_.pop_back();
        if (completed.mapping && !completed.expecting_key)
            return std::unexpected{make_error("scenario.parse.syntax_error", "missing_mapping_value")};
        return attach(std::move(completed.value), completed.pointer, completed.start, completed.own_key);
    }

    auto scalar(fy_event* event) -> std::expected<void, core::Error> {
        fy_token* token = fy_event_get_token(event);
        std::size_t size{};
        const char* bytes = fy_token_get_text(token, &size);
        const std::string_view text{bytes == nullptr ? "" : bytes, size};
        const auto where = location(fy_event_style_start_mark(event));
        if (is_mapping_key()) {
            if (size > max_key) return fail("scenario.parse.key_limit", "mapping_key_bytes", event);
            auto& frame = frames_.back();
            const auto found = frame.keys.find(std::string{text});
            if (found != frame.keys.end()) {
                auto error = make_error("scenario.parse.duplicate_key", "duplicate_key", where, frame.pointer);
                core::detail::JsonCleanup error_cleanup{error.context};
                error.context["first_line"] = found->second.line;
                error.context["first_column"] = found->second.column;
                error.context["duplicate_line"] = where.line;
                error.context["duplicate_column"] = where.column;
                return std::unexpected{std::move(error)};
            }
            if (fy_event_get_node_style(event) == FYNS_PLAIN && text == "<<")
                return fail("scenario.parse.merge_key_not_allowed", "merge_key", event);
            frame.key.assign(text);
            frame.key_location = where;
            frame.keys.emplace(frame.key, where);
            frame.expecting_key = false;
            return {};
        }
        if (size > max_scalar) return fail("scenario.parse.scalar_limit", "scalar_bytes", event);
        auto attachment = next_attachment();
        if (!attachment)
            return std::unexpected{std::move(attachment.error())};
        auto value = fy_event_get_node_style(event) == FYNS_PLAIN ? resolve_plain_scalar(text)
                                                                  : std::expected<nlohmann::ordered_json, core::Error>{std::string{text}};
        if (!value) {
            core::detail::JsonCleanup error_cleanup{value.error().context};
            value.error().context["line"] = where.line;
            value.error().context["column"] = where.column;
            value.error().context["byte_offset"] = where.byte_offset;
            return std::unexpected{std::move(value.error())};
        }
        auto counted = count_node(event);
        if (!counted)
            return std::unexpected{std::move(counted.error())};
        return attach(std::move(*value), attachment->first, where, attachment->second);
    }

    auto count_node(fy_event* event) -> std::expected<void, core::Error> {
        // The document root is the boundary container/value; the shared node
        // budget applies to values contained beneath it. This keeps an array
        // with exactly max_sequence entries admissible.
        if (frames_.empty() && !root_) return {};
        if (nodes_ >= max_nodes) return fail("scenario.parse.node_limit", "total_nodes", event);
        ++nodes_;
        return {};
    }

    auto is_mapping_key() const -> bool {
        return !frames_.empty() && frames_.back().mapping && frames_.back().expecting_key;
    }

    auto current_pointer() const -> std::string { return frames_.empty() ? std::string{} : frames_.back().pointer; }

    auto next_attachment()
        -> std::expected<std::pair<std::string, std::optional<SourceLocation>>, core::Error> {
        if (frames_.empty()) {
            if (root_) return std::unexpected{make_error("scenario.parse.trailing_content", "second_root")};
            return std::pair<std::string, std::optional<SourceLocation>>{"", std::nullopt};
        }
        auto& parent = frames_.back();
        if (parent.mapping) {
            if (parent.expecting_key)
                return std::unexpected{make_error("scenario.parse.complex_key_not_allowed", "missing_scalar_key")};
            if (parent.entries >= max_mapping)
                return std::unexpected{make_error("scenario.parse.mapping_limit", "mapping_entries")};
            return std::pair{parent.pointer + "/" + escape_pointer(parent.key),
                             std::optional<SourceLocation>{parent.key_location}};
        }
        if (parent.entries >= max_sequence)
            return std::unexpected{make_error("scenario.parse.sequence_limit", "sequence_entries")};
        return std::pair{parent.pointer + "/" + std::to_string(parent.entries),
                         std::optional<SourceLocation>{}};
    }

    auto attach(nlohmann::ordered_json value, const std::string& pointer, SourceLocation where,
                std::optional<SourceLocation> key) -> std::expected<void, core::Error> {
        JsonCleanup cleanup{value};
        locations_.emplace(pointer, NodeLocations{.value = where, .mapping_key = key});
        if (frames_.empty()) {
            root_ = std::move(value);
            return {};
        }
        auto& parent = frames_.back();
        if (parent.mapping) {
            append_object(parent.value, parent.key, value);
            parent.key.clear();
            parent.expecting_key = true;
        } else {
            parent.value.push_back(std::move(value));
        }
        ++parent.entries;
        return {};
    }

    std::vector<Frame> frames_;
    std::optional<nlohmann::ordered_json> root_;
    std::map<std::string, NodeLocations> locations_;
    std::size_t nodes_{};
    std::size_t documents_{};
};

} // namespace

auto parse_restricted_yaml(std::string_view contents, bool json_location_mode)
    -> std::expected<ParsedDocument, core::Error> {
    if (!json_location_mode) {
        auto preflight = preflight_explicit_keys(contents);
        if (!preflight)
            return std::unexpected{std::move(preflight.error())};
    }
    auto diag = make_diag();
    if (!diag) return std::unexpected{make_error("scenario.parse.internal_parser_failure", "diagnostic_setup")};
    auto cfg = config(json_location_mode, diag.get());
    ParserPtr parser{fy_parser_create(&cfg)};
    if (!parser || fy_parser_set_string(parser.get(), contents.data(), contents.size()) != 0) {
        return std::unexpected{make_error("scenario.parse.internal_parser_failure", "parser_setup")};
    }
    Builder builder;
    while (fy_event* event = fy_parser_parse(parser.get())) {
        EventGuard guard{parser.get(), event};
        auto result = builder.consume(event);
        if (!result)
            return std::unexpected{std::move(result.error())};
    }
    return builder.finish(fy_parser_get_stream_error(parser.get()));
}

} // namespace epycbench::scenario::detail
