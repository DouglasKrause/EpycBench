#include "results/final_document.hpp"
#include "runtime/finalized_evidence_wire_typed_reconstruction.hpp"
#include "core/json_ownership.hpp"
#include "results/pairwise_analysis_serialization.hpp"
#include "results/pairwise_statistics_calculation.hpp"
#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
namespace epycbench::results::final_document {
namespace {
using J = nlohmann::json;
using SV = std::string_view;
namespace ep = event_profile;
namespace fs = filesystem_journal;
namespace file = platform_linux::journal_file;
[[noreturn]] void reject(Error e) { throw Failure{e, {}, {}, {}}; }
void require(bool condition, Error e = Error::schema) {
    if (!condition) reject(e);
}
auto add(std::size_t a, std::size_t b) -> std::size_t {
    require(b <= SIZE_MAX - a, Error::overflow);
    return a + b;
}
auto mul(std::size_t a, std::size_t b) -> std::size_t {
    require(!a || b <= SIZE_MAX / a, Error::overflow);
    return a * b;
}
struct Tree {
    J value;
    Tree() = default;
    explicit Tree(J&& v) noexcept : value(std::move(v)) {}
    Tree(Tree&&) = default;
    Tree(const Tree&) = delete;
    ~Tree() { core::detail::retire_json(value); }
};
auto string(const J& j) -> const std::string& {
    require(j.is_string());
    return j.get_ref<const std::string&>();
}
auto decimal(const J& j) -> std::uint64_t {
    const auto& s = string(j);
    require(!s.empty() && (s.size() == 1 || s[0] != '0'));
    std::uint64_t v{};
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    require(r.ec == std::errc{} && r.ptr == s.data() + s.size());
    return v;
}
void fields(const J& j, std::initializer_list<SV> names) {
    require(j.is_object() && j.size() == names.size());
    for (auto n : names)
        require(j.contains(n));
}
auto journal_profile(ep::Version version) -> SV {
    switch (version) {
    case ep::Version::supplied_v1: return ep::schema_name;
    case ep::Version::progress_v2: return ep::progress_schema_name;
    case ep::Version::compact_v3: return ep::compact_schema_name;
    }
    reject(Error::schema);
}
auto complete_profile(ep::Version version) -> SV {
    switch (version) {
    case ep::Version::supplied_v1: return "epycbench.private-complete/v1";
    case ep::Version::progress_v2: return "epycbench.private-complete/v2";
    case ep::Version::compact_v3: return "epycbench.private-complete/v3";
    }
    reject(Error::schema);
}
auto document_revision(ep::Version version) -> std::uint64_t {
    switch (version) {
    case ep::Version::supplied_v1: return 0;
    case ep::Version::progress_v2: return 1;
    case ep::Version::compact_v3: return 2;
    }
    reject(Error::schema);
}
auto document_version(const J& root) -> ep::Version {
    const auto& schema = root.at("schema");
    fields(schema, {"name", "major", "revision", "json_schema_dialect"});
    require(string(schema.at("name")) == "epycbench.result" &&
            schema.at("major").is_number_unsigned() && schema.at("major") == 1 &&
            schema.at("revision").is_number_unsigned() &&
            string(schema.at("json_schema_dialect")) ==
                "https://json-schema.org/draft/2020-12/schema");
    const auto& extensions = root.at("extensions");
    fields(extensions, {"epycbench.private_complete"});
    const auto& extension = extensions.at("epycbench.private_complete");
    fields(extension, {"profile", "origin"});
    const auto& journal = root.at("journal");
    require(journal.is_object() && journal.contains("profile"));
    for (const auto version : {ep::Version::supplied_v1, ep::Version::progress_v2,
                               ep::Version::compact_v3})
        if (schema.at("revision") == document_revision(version) &&
            string(journal.at("profile")) == journal_profile(version) &&
            string(extension.at("profile")) == complete_profile(version))
            return version;
    reject(Error::schema);
}
struct Budget {
    const Limits& limits;
    Usage usage;
    std::size_t base{};
    void work(std::size_t n) {
        usage.work = add(usage.work, n);
        require(usage.work <= limits.work, Error::capacity);
    }
    void peak(std::size_t n) {
        n = add(base, n);
        require(n <= limits.simultaneous_bytes, Error::capacity);
        usage.simultaneous_bytes = std::max(usage.simultaneous_bytes, n);
    }
    void cell(std::size_t depth, std::size_t text = 0) {
        usage.nodes = add(usage.nodes, 1);
        usage.text_bytes = add(usage.text_bytes, text);
        usage.depth = std::max(usage.depth, depth);
        require(usage.nodes <= limits.nodes && usage.text_bytes <= limits.text_bytes &&
                    depth <= limits.depth,
                Error::capacity);
        work(add(1, text));
    }
    template <class Json> void tree(const Json& j, std::size_t depth) {
        cell(depth, j.is_string() ? j.template get_ref<const std::string&>().size() : 0U);
        if (j.is_object())
            for (auto it = j.begin(); it != j.end(); ++it) {
                usage.text_bytes = add(usage.text_bytes, it.key().size());
                work(it.key().size());
                tree(it.value(), depth + 1U);
            }
        else if (j.is_array())
            for (const auto& v : j)
                tree(v, depth + 1U);
        require(usage.text_bytes <= limits.text_bytes, Error::capacity);
    }
};
struct Node {
    J& value;
    Budget& budget;
    std::size_t depth;
    auto child(SV key) -> Node {
        budget.cell(depth + 1, key.size());
        auto [it, ok] = value.emplace(std::string(key), nullptr);
        require(ok);
        return {*it, budget, depth + 1};
    }
    auto element() -> Node {
        budget.cell(depth + 1);
        value.push_back(nullptr);
        return {value.back(), budget, depth + 1};
    }
    void object() { value = J::object(); }
    void array() { value = J::array(); }
    auto object(SV key) -> Node {
        auto n = child(key);
        n.object();
        return n;
    }
    auto array(SV key) -> Node {
        auto n = child(key);
        n.array();
        return n;
    }
    void text(SV s) {
        budget.usage.text_bytes = add(budget.usage.text_bytes, s.size());
        require(budget.usage.text_bytes <= budget.limits.text_bytes, Error::capacity);
        budget.work(s.size());
        value = std::string(s);
    }
    void text(SV key, SV s) { child(key).text(s); }
    void number(SV key, std::uint64_t n) { child(key).value = n; }
    void boolean(SV key, bool v) { child(key).value = v; }
    void null(SV key) { (void)child(key); }
    void decimal(SV key, std::uint64_t n) { text(key, std::to_string(n)); }
    void copy(const J& source) {
        budget.tree(source, depth);
        value = core::detail::clone_json(source);
    }
    void copy(SV key, const J& source) { child(key).copy(source); }
    auto present(SV key) -> Node {
        auto n = object(key);
        n.null("absent_reason");
        return n.child("value");
    }
    void absent(SV key, SV reason) {
        auto n = object(key);
        n.null("value");
        n.text("absent_reason", reason);
    }
};
auto parse(SV bytes, Budget& b, std::size_t depth_offset = 0) -> Tree {
    ep::Limits limits;
    limits.body_bytes = b.limits.input_bytes;
    limits.nodes = b.limits.nodes;
    require(depth_offset < b.limits.depth, Error::capacity);
    limits.depth = b.limits.depth - depth_offset;
    limits.text_bytes = b.limits.text_bytes;
    limits.candidate_bytes = b.limits.simultaneous_bytes;
    ep::detail::Work work{0, b.limits.work - b.usage.work, 0, b.limits.simultaneous_bytes - b.base};
    // Admit and retain parse-shape usage before constructing its DOM. Projection
    // counters alone omit the embedded canonical records in the input document.
    const auto shape = ep::detail::shape(bytes, limits, work);
    b.usage.nodes = add(b.usage.nodes, shape.nodes);
    b.usage.text_bytes = add(b.usage.text_bytes, shape.text);
    b.usage.depth = std::max(b.usage.depth, add(shape.depth, depth_offset));
    require(b.usage.nodes <= b.limits.nodes && b.usage.text_bytes <= b.limits.text_bytes &&
                b.usage.depth <= b.limits.depth,
            Error::capacity);
    Tree tree{ep::detail::parse_tree(bytes, limits, work)};
    b.work(work.used);
    b.peak(work.peak);
    return tree;
}
// The original component already owns the validated full typed fact. Build only
// the small presentation view; the original Monitoring capture stays in its event.
auto compact_factual_tree(const block_evidence::Component& component, Budget& b) -> Tree {
    namespace wire = runtime::wire_v1::detail;
    const auto* fact = component.factual();
    require(fact, Error::association);
    const auto admitted = component.usage();
    // Reuse the full component's conservative copy/DOM stocks before a fallible
    // removal-only copy, rather than inventing a smaller allocation contract.
    b.peak(add(sizeof(runtime::FactualObservationEvidence),
               add(admitted.typed_payload, add(admitted.projected_dom,
                                               sizeof(wire::ComponentProjectionCount)))));
    b.work(admitted.wire.nodes);
    const runtime::FactualObservationEvidence small{
        fact->slot, fact->observation_id, fact->observation, fact->validity, {},
        fact->placements, fact->migrations};
    const auto count = wire::count_factual_component(small);
    require(!count.overflow && !count.invalid, Error::capacity);
    const auto dom = wire::component_dom_stock(count.nodes, count.strings);
    require(dom && *dom <= admitted.projected_dom, Error::capacity);
    b.usage.nodes = add(b.usage.nodes, count.nodes);
    b.usage.text_bytes = add(b.usage.text_bytes, count.strings);
    b.usage.depth = std::max(b.usage.depth, count.depth);
    require(b.usage.nodes <= b.limits.nodes && b.usage.text_bytes <= b.limits.text_bytes &&
                b.usage.depth <= b.limits.depth, Error::capacity);
    b.work(add(count.nodes, count.strings));
    return Tree{wire::project_factual_component(small)};
}
// Only canonical getters of an already validated retained Plan use this parser.
// Their historical semantic JSON permits full uint64 values. The untrusted final
// document and event grammars keep the stricter C2 numeric admission unchanged.
struct PlanShape final : nlohmann::json_sax<J> {
    Budget& budget;
    std::size_t nesting{}, nodes{}, text{};
    explicit PlanShape(Budget& b) : budget(b) {}
    auto node(std::size_t bytes = 0) -> bool {
        budget.cell(nesting + 1U, bytes);
        nodes = add(nodes, 1U); text = add(text, bytes); return true;
    }
    auto null() -> bool override { return node(); }
    auto boolean(bool) -> bool override { return node(); }
    auto number_integer(number_integer_t) -> bool override { return node(); }
    auto number_unsigned(number_unsigned_t) -> bool override { return node(); }
    auto number_float(number_float_t value, const string_t&) -> bool override {
        require(std::isfinite(value)); return node();
    }
    auto string(string_t& value) -> bool override { return node(value.size()); }
    auto binary(binary_t&) -> bool override { reject(Error::schema); }
    auto start_object(std::size_t) -> bool override { node(); ++nesting; return true; }
    auto start_array(std::size_t) -> bool override { node(); ++nesting; return true; }
    auto key(string_t& value) -> bool override {
        budget.cell(nesting + 1U, value.size());
        nodes = add(nodes, 1U); text = add(text, value.size()); return true;
    }
    auto end_object() -> bool override { --nesting; return true; }
    auto end_array() -> bool override { --nesting; return true; }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) -> bool override {
        reject(Error::schema);
    }
};
struct PlanBuilder final : nlohmann::json_sax<J> {
    struct Frame { J* value{}; std::string key; };
    Tree root;
    std::array<Frame, 64> frames{};
    std::size_t depth{};
    auto destination() -> J& {
        if (!depth) return root.value;
        auto& frame = frames[depth - 1U];
        if (frame.value->is_array()) { frame.value->push_back(nullptr); return frame.value->back(); }
        return (*frame.value)[frame.key];
    }
    template<class T> auto scalar(T&& value) -> bool {
        destination() = std::forward<T>(value); return true;
    }
    auto null() -> bool override { return scalar(nullptr); }
    auto boolean(bool value) -> bool override { return scalar(value); }
    auto number_integer(number_integer_t value) -> bool override { return scalar(value); }
    auto number_unsigned(number_unsigned_t value) -> bool override { return scalar(value); }
    auto number_float(number_float_t value, const string_t&) -> bool override {
        require(std::isfinite(value)); return scalar(value);
    }
    auto string(string_t& value) -> bool override { return scalar(std::move(value)); }
    auto binary(binary_t&) -> bool override { reject(Error::schema); }
    auto begin(bool object) -> bool {
        require(depth < frames.size(), Error::capacity);
        auto& value = destination(); value = object ? J::object() : J::array();
        frames[depth++].value = &value;
        return true;
    }
    auto start_object(std::size_t) -> bool override { return begin(true); }
    auto start_array(std::size_t) -> bool override { return begin(false); }
    auto key(string_t& value) -> bool override {
        require(depth && !frames[depth - 1U].value->contains(value));
        frames[depth - 1U].key = std::move(value); return true;
    }
    auto end_object() -> bool override { --depth; return true; }
    auto end_array() -> bool override { --depth; return true; }
    auto parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) -> bool override {
        reject(Error::schema);
    }
};
auto trusted_plan_tree(SV bytes, Budget& b) -> Tree {
    require(bytes.size() <= b.limits.input_bytes, Error::capacity);
    const auto lexical = add(sizeof(PlanShape), mul(add(bytes.size(), 1U), 4U));
    b.peak(lexical); b.work(mul(bytes.size(), 2U));
    PlanShape shape{b};
    require(J::sax_parse(bytes, &shape));
    // Container cells, geometric growth, keys and lexical buffers coexist.
    const auto cells = mul(shape.nodes, add(mul(sizeof(J), 4U),
                                          add(sizeof(std::string), 4U * sizeof(void*))));
    b.peak(add(lexical, add(sizeof(PlanBuilder), add(cells, mul(shape.text, 4U)))));
    PlanBuilder builder;
    require(J::sax_parse(bytes, &builder));
    return Tree{std::move(builder.root.value)};
}
auto render(const J& j, Budget& b, std::size_t maximum = SIZE_MAX) -> std::string {
    const auto output_bytes = std::min(b.limits.output_bytes, maximum);
    b.peak(add(mul(output_bytes, 2), mul(b.limits.nodes, sizeof(J))));
    auto value =
        core::canonicalize_json_jcs(j, {output_bytes, b.limits.depth, b.limits.nodes});
    core::detail::ExpectedErrorCleanup cleanup{value};
    require(value.has_value(), Error::canonical);
    b.work(value->size());
    return std::move(*value);
}
auto hex(SV source, Budget& b) -> std::string {
    const auto n = mul(source.size(), 2);
    b.work(n);
    require(n <= b.limits.text_bytes, Error::capacity);
    std::string out(n, '0');
    constexpr SV digits = "0123456789abcdef";
    for (std::size_t i{}; i < source.size(); ++i) {
        auto c = static_cast<unsigned char>(source[i]);
        out[2 * i] = digits[c >> 4];
        out[2 * i + 1] = digits[c & 15U];
    }
    return out;
}
void artifact(Node target, SV profile, SV canonical, const core::Sha256Digest& digest) {
    target.object();
    target.text("profile", profile);
    target.text("canonical", canonical);
    target.text("digest", digest.to_hex());
}
void artifact(Node parent, SV key, const auto* owner, SV profile) {
    if (!owner) {
        parent.absent(key, "not_obtained");
        return;
    }
    artifact(parent.present(key), profile, owner->canonical(), *owner->digest());
}
auto logarithmic(std::size_t n) -> std::size_t {
    return mul(n, static_cast<std::size_t>(std::bit_width(std::max(std::size_t{2}, n) - 1U)));
}
struct MathStock {
    std::size_t peak{}, retained{}, copied{}, work{}, blocks{};
};
auto math_stock(const ep::detail::ComposedPair& pair, std::optional<std::uint64_t> boundary = {})
    -> MathStock {
    MathStock out;
    if (!pair.prefix || !pair.context)
        return out;
    const auto& prefix = *pair.prefix;
    std::size_t copied = sizeof(std::vector<runtime::PairwiseBlockAttemptEvidence>), events{};
    for (std::size_t i{}; i < prefix.block_count(); ++i) {
        const auto rows = prefix.events(i);
        events = add(events, rows.size());
        if (rows.empty() || rows.back().kind != block_evidence::EventKind::closed ||
            rows.back().completion != runtime::BlockAttemptCompletion::complete_valid)
            continue;
        require(rows.front().component.plan() != nullptr, Error::association);
        if (boundary && rows.front().component.plan()->block_id().value() > *boundary)
            continue;
        ++out.blocks;
        copied = add(copied, add(sizeof(runtime::PairwiseBlockAttemptEvidence),
                                 mul(4, sizeof(runtime::FactualObservationEvidence))));
        for (const auto& row : rows)
            if (row.component.available())
                copied = add(copied, row.component.usage().typed_payload);
    }
    if (!out.blocks)
        return out;
    const auto& policy = pair.context->header.statistics_policy;
    const auto raw_replicates =
        (boundary ? policy.stopping_bootstrap_policy() : policy.comparison_bootstrap_policy())
            .replicate_count()
            .value();
    require(raw_replicates <= SIZE_MAX, Error::capacity);
    const auto r = static_cast<std::size_t>(raw_replicates), b = out.blocks, n = mul(2, b);
    require(r <= std::vector<double>{}.max_size() &&
                b <= std::vector<statistics::PairedLogEffect>{}.max_size() &&
                n <= std::vector<core::ObservationId>{}.max_size(),
            Error::capacity);
    const auto metric =
        std::max(sizeof(statistics::Bandwidth), sizeof(statistics::DependentLoadLatency));
    const auto population =
        add(add(mul(mul(2, n), metric), mul(mul(2, n), sizeof(core::ObservationId))),
            mul(b, add(sizeof(kernel::DependentLoadLatencyBlockId),
                       sizeof(statistics::PairedLogEffect))));
    const auto retained = add(add(mul(mul(2, n), sizeof(core::ObservationId)),
                                  mul(b, add(sizeof(kernel::DependentLoadLatencyBlockId),
                                             sizeof(statistics::PairedLogEffect)))),
                              mul(mul(2, n), sizeof(statistics::OutlierFlagState)));
    const auto desc = mul(mul(2, n), sizeof(double));
    const auto flags = add(desc, mul(n, sizeof(statistics::OutlierFlagState)));
    const auto bootstrap = add(mul(b, add(sizeof(statistics::PairedLogEffect), sizeof(double))),
                               mul(r, sizeof(double)));
    // Explicit fixed logical cells from the reviewed worksheet; no stack/ABI claim.
    const auto summaries = mul(
        2, std::max(sizeof(statistics::DescriptiveStatistics<statistics::Bandwidth>),
                    sizeof(statistics::DescriptiveStatistics<statistics::DependentLoadLatency>)));
    const auto outliers = mul(
        2, std::max(sizeof(statistics::OutlierFlaggingResult<statistics::Bandwidth>),
                    sizeof(statistics::OutlierFlaggingResult<statistics::DependentLoadLatency>)));
    const auto fixed =
        add(sizeof(PairwiseStatistics),
            add(add(summaries, outliers), add(mul(6, sizeof(std::vector<double>)),
                                              add(mul(4, sizeof(FinalCalculationFailure)),
                                                  sizeof(statistics::LogRatioComparison) +
                                                      sizeof(statistics::BlockBootstrapResult) +
                                                      sizeof(statistics::ComparisonConclusion)))));
    out.retained = add(fixed, retained);
    out.copied = copied;
    out.peak = add(add(add(fixed, copied), population),
                   add(retained, std::max({desc, flags, mul(b, sizeof(double)), bootstrap})));
    out.work = add(copied, add(events, add(mul(2, prefix.block_count()), mul(16, b))));
    out.work = add(out.work, mul(2, add(mul(4, n), logarithmic(n))));
    out.work = add(out.work, mul(2, add(mul(3, n), mul(2, logarithmic(n)))));
    out.work = add(out.work, add(mul(2, b), logarithmic(b)));
    out.work =
        add(out.work, add(mul(r, add(add(mul(3, b), logarithmic(b)), 1)), add(logarithmic(r), 16)));
    return out;
}
void preflight_math(const ep::Transaction& tx, Budget& b) {
    const auto* state = tx.composition();
    if (!state)
        return;
    for (const auto& pair : state->pairs) {
        if (!pair)
            continue;
        auto stock = math_stock(*pair);
        b.peak(stock.peak);
        b.work(stock.work);
        if (pair->prefix)
            for (const auto& stop : pair->prefix->stopping_events()) {
                stock = math_stock(*pair, stop.evidence.boundary_block_id.value());
                b.peak(stock.peak);
                b.work(stock.work);
            }
    }
}
void reference(Node n, const ep::Record& r, SV kind = {}, std::uint64_t ordinal = 0) {
    n.budget.usage.references = add(n.budget.usage.references, 1);
    require(n.budget.usage.references <= n.budget.limits.references, Error::capacity);
    n.object();
    n.decimal("sequence", r.sequence);
    n.decimal("ordinal", ordinal);
    n.text("kind", kind.empty() ? ep::detail::event_name(r.kind) : kind);
}
void issue_json(Node n, const file::Issue& issue) {
    n.object();
    n.number("phase", static_cast<unsigned>(issue.phase));
    n.number("reason", static_cast<unsigned>(issue.reason));
    n.child("native_error").value = issue.native_error;
}
void cleanup_json(Node n, const file::CloseReport& c) {
    n.object();
    n.decimal("attempts", c.attempts);
    auto rows = n.array("issues");
    for (std::size_t i{}; i < c.count; ++i)
        issue_json(rows.element(), c.issues[i]);
}
void file_failure_json(Node n, const file::Failure& f) {
    n.object();
    issue_json(n.child("primary"), f.primary);
    cleanup_json(n.child("cleanup"), f.cleanup);
    n.boolean("created", f.created);
}
void failure_json(Node n, const fs::Failure& f) {
    n.object();
    n.number("kind", static_cast<unsigned>(f.kind));
    if (f.semantic)
        n.number("semantic", static_cast<unsigned>(*f.semantic));
    else
        n.null("semantic");
    if (f.io)
        file_failure_json(n.child("io"), *f.io);
    else
        n.null("io");
}
void position_json(Node n, const fs::Position& p) {
    n.object();
    n.decimal("records", p.records);
    n.decimal("bytes", p.bytes);
    if (p.digest)
        n.text("digest", p.digest->to_hex());
    else
        n.null("digest");
}
void summary_json(Node n, const ep::Summary& s) {
    n.object();
    n.decimal("records", s.records);
    if (s.digest)
        n.text("digest", s.digest->to_hex());
    else
        n.null("digest");
    n.number("tail", static_cast<unsigned>(s.tail));
    n.number("recovery", static_cast<unsigned>(s.recovery));
    n.decimal("fragment_bytes", s.fragment_bytes);
}
void receipt_json(Node n, const fs::Receipt& r) {
    n.object();
    n.text("verification_scope", "reported_source_receipt");
    n.text("original_tail_verification", "not_reproduced");
    n.number("state", static_cast<unsigned>(r.state));
    position_json(n.child("semantic"), r.semantic);
    position_json(n.child("complete"), r.complete);
    if (r.synchronized)
        position_json(n.child("synchronized"), *r.synchronized);
    else
        n.null("synchronized");
    n.decimal("written_bytes", r.written_bytes);
    n.decimal("partial_frame_bytes", r.partial_frame_bytes);
    n.boolean("directory_synchronized", r.directory_synchronized);
    n.boolean("file_eof", r.file_eof);
    n.boolean("persistence_uncertain", r.persistence_uncertain);
    if (r.recovery)
        summary_json(n.child("recovery"), *r.recovery);
    else
        n.null("recovery");
    if (r.failure)
        failure_json(n.child("failure"), *r.failure);
    else
        n.null("failure");
    if (r.cleanup) {
        auto c = n.object("cleanup");
        c.decimal("attempts", r.cleanup->attempts);
        if (r.cleanup->primary)
            failure_json(c.child("primary"), *r.cleanup->primary);
        else
            c.null("primary");
        auto rows = c.array("secondary");
        for (std::size_t i{}; i < r.cleanup->count; ++i)
            issue_json(rows.element(), r.cleanup->secondary[i]);
    } else
        n.null("cleanup");
}
auto unsigned_number(const J& j, std::uint64_t max) -> std::uint64_t {
    require(j.is_number_unsigned());
    const auto v = j.get<std::uint64_t>();
    require(v <= max);
    return v;
}
auto boolean(const J& j) -> bool {
    require(j.is_boolean());
    return j.get<bool>();
}
auto digest_value(const J& j) -> std::optional<core::Sha256Digest> {
    if (j.is_null())
        return {};
    const auto& text = string(j);
    require(text.size() == 64);
    auto parsed = core::Sha256Digest::parse(text);
    core::detail::ExpectedErrorCleanup cleanup{parsed};
    require(parsed.has_value());
    return *parsed;
}
auto parse_issue(const J& j) -> file::Issue {
    fields(j, {"phase", "reason", "native_error"});
    require(j.at("native_error").is_number_integer());
    const auto raw = j.at("native_error").get<std::int64_t>();
    require(raw >= 0 && raw <= std::numeric_limits<int>::max());
    return {static_cast<file::Phase>(
                unsigned_number(j.at("phase"), static_cast<unsigned>(file::Phase::close))),
            static_cast<file::Reason>(
                unsigned_number(j.at("reason"), static_cast<unsigned>(file::Reason::mutation))),
            static_cast<int>(raw)};
}
auto parse_close(const J& j) -> file::CloseReport {
    fields(j, {"attempts", "issues"});
    file::CloseReport out;
    out.attempts = decimal(j.at("attempts"));
    require(out.attempts <= 2);
    const auto& rows = j.at("issues");
    require(rows.is_array() && rows.size() <= out.attempts);
    for (const auto& row : rows)
        out.issues[out.count++] = parse_issue(row);
    return out;
}
auto parse_failure(const J& j) -> fs::Failure {
    fields(j, {"kind", "semantic", "io"});
    fs::Failure out;
    out.kind = static_cast<fs::Error>(
        unsigned_number(j.at("kind"), static_cast<unsigned>(fs::Error::mutation)));
    if (!j.at("semantic").is_null())
        out.semantic = static_cast<ep::Error>(
            unsigned_number(j.at("semantic"), static_cast<unsigned>(ep::Error::unsupported)));
    if (!j.at("io").is_null()) {
        const auto& v = j.at("io");
        fields(v, {"primary", "cleanup", "created"});
        out.io = file::Failure{parse_issue(v.at("primary")), parse_close(v.at("cleanup")),
                               boolean(v.at("created"))};
    }
    require((out.kind == fs::Error::semantic) == out.semantic.has_value());
    require((out.kind == fs::Error::io) == out.io.has_value());
    return out;
}
auto parse_position(const J& j) -> fs::Position {
    fields(j, {"records", "bytes", "digest"});
    return {decimal(j.at("records")), digest_value(j.at("digest")),
            static_cast<std::size_t>(decimal(j.at("bytes")))};
}
auto parse_summary(const J& j) -> ep::Summary {
    fields(j, {"records", "digest", "tail", "recovery", "fragment_bytes"});
    return {decimal(j.at("records")), digest_value(j.at("digest")),
            static_cast<ep::Tail>(unsigned_number(j.at("tail"), 2)),
            static_cast<ep::Recovery>(unsigned_number(j.at("recovery"), 2)),
            static_cast<std::size_t>(decimal(j.at("fragment_bytes")))};
}
auto parse_receipt(const J& j) -> fs::Receipt {
    fields(j, {"verification_scope", "original_tail_verification", "state", "semantic", "complete",
               "synchronized", "written_bytes", "partial_frame_bytes", "directory_synchronized",
               "file_eof", "persistence_uncertain", "recovery", "failure", "cleanup"});
    require(string(j.at("verification_scope")) == "reported_source_receipt" &&
            string(j.at("original_tail_verification")) == "not_reproduced");
    fs::Receipt out;
    out.state = static_cast<fs::State>(unsigned_number(j.at("state"), 4));
    require(out.state != fs::State::moved_from);
    out.semantic = parse_position(j.at("semantic"));
    out.complete = parse_position(j.at("complete"));
    if (!j.at("synchronized").is_null())
        out.synchronized = parse_position(j.at("synchronized"));
    out.written_bytes = decimal(j.at("written_bytes"));
    out.partial_frame_bytes = decimal(j.at("partial_frame_bytes"));
    out.directory_synchronized = boolean(j.at("directory_synchronized"));
    out.file_eof = boolean(j.at("file_eof"));
    out.persistence_uncertain = boolean(j.at("persistence_uncertain"));
    if (!j.at("recovery").is_null())
        out.recovery = parse_summary(j.at("recovery"));
    if (!j.at("failure").is_null())
        out.failure = parse_failure(j.at("failure"));
    if (!j.at("cleanup").is_null()) {
        const auto& c = j.at("cleanup");
        fields(c, {"primary", "secondary", "attempts"});
        fs::CloseReport report;
        report.attempts = decimal(c.at("attempts"));
        require(report.attempts <= 2);
        if (!c.at("primary").is_null())
            report.primary = parse_failure(c.at("primary"));
        const auto& rows = c.at("secondary");
        require(rows.is_array() && rows.size() <= 2);
        for (const auto& row : rows)
            report.secondary[report.count++] = parse_issue(row);
        out.cleanup = report;
    }
    require(out.complete.records <= out.semantic.records &&
            out.complete.bytes <= out.semantic.bytes);
    if (out.synchronized)
        require(out.synchronized->records <= out.complete.records &&
                out.synchronized->bytes <= out.complete.bytes);
    require(!out.file_eof || out.recovery.has_value());
    return out;
}
void check_receipt(const fs::Receipt& r, const ep::Transaction& tx, std::size_t frames, Budget& b) {
    const auto summary = tx.summary();
    const auto position = [&](const fs::Position& p) {
        require(p.records <= tx.records().size(), Error::association);
        std::size_t bytes{};
        std::optional<core::Sha256Digest> digest;
        b.work(static_cast<std::size_t>(p.records));
        for (std::size_t i{}; i < p.records; ++i) {
            bytes = add(bytes, add(tx.records()[i].canonical.size(), 2U));
            digest = tx.records()[i].digest;
        }
        require(p.bytes == bytes && p.digest == digest, Error::association);
    };
    position(r.semantic); position(r.complete);
    if (r.synchronized) position(*r.synchronized);
    require(r.semantic.records == summary.records && r.semantic.digest == summary.digest &&
            r.semantic.bytes == frames && r.complete.records <= r.semantic.records,
            Error::association);
    require(r.cleanup.has_value() == (r.state == fs::State::closed), Error::association);
    if (r.recovery) {
        require(r.recovery->records == summary.records && r.recovery->digest == summary.digest &&
                r.complete.records == r.semantic.records && !r.synchronized &&
                !r.written_bytes && !r.partial_frame_bytes && !r.directory_synchronized &&
                !r.persistence_uncertain, Error::association);
        require(r.state == fs::State::closed ||
                r.state == (r.failure ? fs::State::failed : fs::State::sealed), Error::association);
        require(!r.file_eof || !r.failure || r.failure->kind == fs::Error::io, Error::association);
        if (!r.failure)
            require(r.file_eof && r.recovery->tail != ep::Tail::corrupt, Error::association);
    } else {
        require((r.state == fs::State::sealed || r.state == fs::State::closed) && !r.failure &&
                    r.complete.records == summary.records && r.written_bytes == frames &&
                    !r.partial_frame_bytes && !r.file_eof && !r.persistence_uncertain,
                Error::association);
        require(r.synchronized.has_value() == r.directory_synchronized &&
                    (!r.synchronized || r.synchronized->records == r.complete.records),
                Error::association);
    }
}

enum class ArtifactSelector {
    creation_host, normalization, planning_execution, topology,
    normalization_topology, creation_host_topology, planning_object_evidence
};
auto artifact_selector(ArtifactSelector selector) -> SV {
    switch (selector) {
    case ArtifactSelector::creation_host: return "creation.host";
    case ArtifactSelector::normalization: return "normalization";
    case ArtifactSelector::planning_execution: return "planning.execution";
    case ArtifactSelector::topology: return "topology";
    case ArtifactSelector::normalization_topology: return "normalization.topology";
    case ArtifactSelector::creation_host_topology: return "creation.host.topology";
    case ArtifactSelector::planning_object_evidence: return "planning.object_evidence";
    }
    reject(Error::reference);
}
void artifact_reference(Node n, const ep::Record& source, ArtifactSelector selector,
                        SV profile, const core::Sha256Digest& digest) {
    n.object();
    n.text("representation", "embedded_event_artifact");
    reference(n.child("source"), source);
    n.text("selector", artifact_selector(selector));
    n.text("profile", profile);
    n.text("digest", digest.to_hex());
}
struct Sources {
    const run_creation_serialization::Owner* genesis{};
    const normalization_serialization::Owner* normalized{};
    const source_serialization::Owner* source{};
    const topology_serialization::Owner* topology{};
    const ep::detail::CompositionState* composition{};
    const ep::Record *genesis_record{}, *normalization_record{}, *topology_record{},
        *planning_record{};
    ArtifactSelector topology_selector{ArtifactSelector::topology};
    bool compact{};
};
auto sources(const ep::Transaction& tx) -> Sources {
    require(!tx.records().empty() && tx.records().front().kind == ep::Kind::creation,
            Error::missing_genesis);
    Sources out;
    out.compact = tx.version() == ep::Version::compact_v3;
    out.genesis_record = &tx.records().front();
    out.genesis = std::get_if<run_creation_serialization::Owner>(&tx.records().front().artifact);
    require(out.genesis && out.genesis->provenance(), Error::association);
    require(out.genesis->provenance()->domain == process_subject::Domain::synthetic_execution ||
            (ep::is_progress(tx.records().front().version) &&
             out.genesis->provenance()->domain == process_subject::Domain::supplied_observation),
            Error::association);
    out.source = out.genesis->source();
    out.composition = tx.composition();
    for (const auto& r : tx.records()) {
        if (const auto* n = std::get_if<normalization_serialization::Owner>(&r.artifact)) {
            out.normalized = n;
            out.normalization_record = &r;
            out.source = n->source();
            out.topology = n->topology();
            out.topology_record = &r;
            out.topology_selector = ArtifactSelector::normalization_topology;
        }
        if (const auto* t = std::get_if<topology_serialization::Owner>(&r.artifact)) {
            out.topology = t;
            out.topology_record = &r;
            out.topology_selector = ArtifactSelector::topology;
        }
        if (const auto* n = std::get_if<source_outcome_serialization::Owner>(&r.artifact))
            out.source = n->source();
    }
    if (!out.topology && out.genesis->host()) {
        out.topology = out.genesis->host()->topology();
        out.topology_record = out.genesis_record;
        out.topology_selector = ArtifactSelector::creation_host_topology;
    }
    if (out.composition && out.composition->planning) {
        const auto sequence = out.composition->planning_record;
        require(sequence && sequence <= tx.records().size(), Error::reference);
        out.planning_record = &tx.records()[static_cast<std::size_t>(sequence - 1U)];
        require(out.planning_record->kind == ep::Kind::planning, Error::reference);
    }
    return out;
}
// This is a Results view of a validated Scenario tree, not a new Scenario owner.
// Opaque metadata and Kernel parameters are handled separately below.
void normalized_numbers(J& value, Budget& b) {
    b.work(1);
    if (value.is_array()) {
        for (auto& item : value) normalized_numbers(item, b);
        return;
    }
    if (!value.is_object()) return;
    const auto unit = value.find("unit");
    const bool quantity = unit != value.end() && unit->is_string() &&
        (string(*unit) == "byte" || string(*unit) == "nanosecond" || string(*unit) == "ratio");
    for (auto it = value.begin(); it != value.end(); ++it) {
        const auto& key = it.key();
        if (key == "metadata" || key == "workloads") continue;
        const bool exact = (quantity && (key == "value" || key == "numerator" || key == "denominator")) ||
            key == "seed" || key == "minimum_cpu_nodes" || key == "minimum_memory_nodes" ||
            key == "minimum_blocks" || key == "maximum_blocks" ||
            key == "minimum_valid_observations_per_variant" || key == "minimum" ||
            key == "maximum" || key == "stability_window" || key == "bootstrap_samples" || key == "count";
        if (exact && it->is_number_unsigned()) {
            b.cell(1, 20); // decimal temporary admitted before allocation
            *it = std::to_string(it->get<std::uint64_t>());
        } else normalized_numbers(*it, b);
    }
}
void source_projection(Node n, const Sources& s, Budget& b) {
    artifact(n, "source", s.source, source_serialization::profile);
    if (s.compact && s.normalized) {
        require(s.normalization_record && s.normalized->digest(), Error::reference);
        artifact_reference(n.present("normalization"), *s.normalization_record,
                           ArtifactSelector::normalization,
                           normalization_serialization::profile, *s.normalized->digest());
    } else {
        artifact(n, "normalization", s.normalized, normalization_serialization::profile);
    }
    if (s.source && s.source->source()) {
        const auto value = s.source->source()->input();
        if (value.availability == authored_source::Availability::available) {
            n.present("authored").text(hex(value.bytes, b));
            n.present("source_format")
                .text(value.format == scenario::ScenarioFormat::json ? "json" : "yaml");
            n.present("source_name").text(hex(value.name, b));
            auto overrides = n.present("overrides");
            overrides.array();
            for (auto item : value.rejected_overrides)
                overrides.element().text(hex(item, b));
        } else
            for (auto key : {"authored", "source_format", "source_name", "overrides"})
                n.absent(key, value.availability == authored_source::Availability::not_attempted
                                  ? "not_attempted"
                                  : "not_obtained");
    } else
        for (auto key : {"authored", "source_format", "source_name", "overrides"})
            n.absent(key, "not_obtained");
    if (s.normalized && s.normalized->plan()) {
        const auto* plan = s.normalized->plan();
        const auto* ancestry = plan->document_ancestry();
        require(ancestry && ancestry->default_expanded(), Error::association);
        b.tree(*ancestry->default_expanded(), 1);
        Tree normalized{core::detail::clone_json_as<J>(*ancestry->default_expanded())};
        normalized_numbers(normalized.value, b);
        for (auto& workload : normalized.value.at("workloads")) {
            if (!workload.contains("parameters")) continue;
            auto& parameters = workload.at("parameters");
            if (parameters.contains("chain_seed")) {
                auto& seed = parameters.at("chain_seed");
                require(seed.is_number_unsigned());
                b.cell(1, 20);
                seed = std::to_string(seed.get<std::uint64_t>());
            }
        }
        auto& metadata = normalized.value.at("metadata");
        if (metadata.contains("user")) {
            require(metadata.at("user").is_object() && ancestry->digest() && s.normalized->digest(),
                    Error::association);
            core::detail::retire_json(metadata.at("user"));
            Node reference{metadata.at("user"), b, 5};
            reference.object();
            reference.text("representation", "embedded_document_ancestry");
            reference.text("artifact_pointer", "/scenario/normalization/value");
            reference.text("normalization_digest", s.normalized->digest()->to_hex());
            reference.text("ancestry_digest", ancestry->digest()->to_hex());
            reference.text("tree", "default_expanded");
            reference.text("pointer", "/metadata/user");
        }
        n.present("normalized").copy(normalized.value);
        n.present("scenario_canonical").text(plan->canonical_scenario());
        n.present("scenario_digest").text(plan->scenario_digest()->text());
    } else
        for (auto key : {"normalized", "scenario_canonical", "scenario_digest"})
            n.absent(key, "not_obtained");
}
void plan_projection(Node n, const Sources& s) {
    const auto* plan = s.normalized ? s.normalized->plan() : nullptr;
    artifact(n, "resolved", plan, private_plan_composition::profile);
    if (plan) {
        n.present("resolved_canonical").text(plan->canonical_resolved_plan());
        n.present("resolved_digest").text(plan->resolved_digest()->text());
    } else {
        n.absent("resolved_canonical", "plan_unavailable");
        n.absent("resolved_digest", "plan_unavailable");
    }
    const auto* execution = s.composition ? s.composition->planning.get() : nullptr;
    if (execution) {
        if (s.compact) {
            require(s.planning_record, Error::reference);
            artifact_reference(n.present("execution"), *s.planning_record,
                               ArtifactSelector::planning_execution,
                               execution_plan::artifact_profile, execution->digest);
        } else {
            artifact(n.present("execution"), execution_plan::artifact_profile,
                     execution->canonical, execution->digest);
        }
        n.present("execution_canonical").text(execution->semantic);
        n.present("execution_digest").text(execution->execution_digest.to_hex());
        auto pairs = n.present("pairs");
        pairs.array();
        for (const auto& p : execution->pairs) {
            auto r = pairs.element();
            r.object();
            r.number("ordinal", p.ordinal);
            r.number("experiment_index", p.experiment_index);
            r.number("child_experiment_index", p.child_experiment_index);
            r.number("workload_index", p.workload_index);
            r.number("baseline_index", p.baseline_index);
            r.number("candidate_index", p.candidate_index);
            r.text("kind",
                   p.kind == execution_plan::PairKind::comparison ? "comparison" : "matrix");
            r.number("order", static_cast<unsigned>(p.order));
            r.decimal("order_seed", p.order_seed);
        }
        auto c = n.present("counts");
        c.object();
        const auto& x = execution->counts;
        c.decimal("minimum_blocks", x.minimum_blocks);
        c.decimal("maximum_blocks", x.maximum_blocks);
        c.decimal("minimum_valid_per_variant", x.minimum_valid_per_variant);
        c.decimal("maximum_per_variant", x.maximum_per_variant);
        c.decimal("root_seed", x.root_seed);
        c.decimal("maximum_observations_low", x.maximum_observations.low);
        c.boolean("maximum_observations_two_to_64", x.maximum_observations.two_to_64);
        c.boolean("stopping_enabled", x.stopping_enabled);
        c.boolean("monitoring_present", x.monitoring_present);
    } else
        for (auto key : {"execution", "execution_canonical", "execution_digest", "pairs", "counts"})
            n.absent(key, "plan_unavailable");
}
auto projected_status(const ep::Transaction& tx, const fs::Receipt& receipt) -> std::array<SV, 5> {
    SV observed = "planned", validity = "not_evaluated", conclusion = "not_evaluated";
    const auto* c = tx.composition();
    if (c) {
        if (std::ranges::any_of(c->pairs, [](const auto& p) { return bool(p); }))
            observed = "running";
        if (c->closed_record)
            observed = string(
                tx.records()[static_cast<std::size_t>(c->closed_record - 1U)].event_facts->at(
                    "execution"));
        bool evaluated = false, invalid = false;
        for (const auto& pair : c->pairs)
            if (pair && pair->analysis) {
                evaluated =
                    evaluated || pair->analysis->validity() == PairwiseResultValidity::valid;
                invalid = invalid || pair->analysis->validity() == PairwiseResultValidity::invalid;
            }
        if (invalid)
            validity = "invalid";
        else if (evaluated && observed == "completed" &&
                 std::ranges::all_of(c->pairs, [](const auto& p) {
                     return p && p->analysis &&
                            p->analysis->validity() == PairwiseResultValidity::valid;
                 }))
            validity = "valid";
    }
    const auto summary = receipt.recovery ? *receipt.recovery : tx.summary();
    SV disposition = summary.recovery == ep::Recovery::completed ? "completed"
                     : summary.recovery == ep::Recovery::failed  ? "failed"
                                                                 : "interrupted";
    if (receipt.failure || summary.tail == ep::Tail::corrupt)
        disposition = "failed";
    SV execution = disposition == "failed"                           ? "failed"
                   : observed == "completed" || observed == "failed" ? observed
                                                                     : "interrupted";
    if (validity == "invalid")
        conclusion = "invalid";
    if (c && c->pairs.size() == 1 && c->pairs[0] && c->pairs[0]->analysis &&
        execution == "completed" && validity == "valid") {
        const auto& stats = c->pairs[0]->analysis->authoritative_statistics();
        if (stats)
            std::visit(
                [&](const auto& v) {
                    constexpr std::array<SV, 4> names{"candidate_faster", "baseline_faster",
                                                      "detectable_below_practical_threshold",
                                                      "inconclusive"};
                    conclusion = names[static_cast<std::size_t>(v.conclusion)];
                },
                *stats);
    }
    if (execution != "completed" && validity == "valid")
        validity = "not_evaluated";
    return {execution, validity, conclusion, observed, disposition};
}
void diagnostics(Node n, const ep::Transaction& tx) {
    for (const auto& r : tx.records()) {
        if (r.kind != ep::Kind::failure && r.kind != ep::Kind::source_outcome &&
            r.kind != ep::Kind::creation)
            continue;
        const auto emit = [&](SV canonical, const core::Sha256Digest& digest, SV profile,
                              std::uint64_t ordinal,
                              const error_serialization::Provenance* child = nullptr) {
            auto row = n.element();
            row.object();
            row.text("id", std::string{sources(tx).genesis->provenance()->run} + ":" +
                               std::to_string(r.sequence) +
                               ":diagnostic:" + std::to_string(ordinal));
            reference(row.child("source"), r,
                      child && r.kind == ep::Kind::creation ? "creation_failure" : SV{}, ordinal);
            auto primary = r.primary;
            if (child && child->association && child->association->secondary_creation_ordinal)
                primary = ep::Reference{r.sequence, *child->association->secondary_creation_ordinal,
                                        ep::RefKind::creation_failure};
            if (r.event_facts && r.kind == ep::Kind::failure) {
                row.copy("role", r.event_facts->at("role"));
                row.copy("primary_ref", r.event_facts->at("primary_ref"));
            } else {
                row.text("role", primary ? "secondary" : "primary");
                if (primary) {
                    auto ref = row.object("primary_ref");
                    ref.decimal("sequence", primary->sequence);
                    ref.decimal("ordinal", primary->ordinal);
                    constexpr std::array<SV, 5> kinds{"run_created", "topology_retained",
                                                      "creation_failure", "ordinary_failure",
                                                      "source_resolution_failure"};
                    ref.text("kind", kinds[static_cast<std::size_t>(primary->kind)]);
                } else
                    row.null("primary_ref");
            }
            auto scope = row.object("scope");
            const std::array<std::pair<SV, std::optional<std::uint64_t>>, 4> scopes{
                {{"experiment", r.scope.experiment},
                 {"pair", r.scope.pair},
                 {"block", r.scope.block},
                 {"observation", r.scope.observation}}};
            for (const auto& [key, value] : scopes)
                if (value)
                    scope.decimal(key, *value);
                else
                    scope.null(key);
            artifact(row.child("artifact"), profile, canonical, digest);
        };
        if (const auto* value = std::get_if<error_serialization::Owner>(&r.artifact))
            emit(value->canonical(), *value->digest(), error_serialization::profile, 0);
        else if (const auto* creation_value =
                     std::get_if<run_creation_serialization::Owner>(&r.artifact)) {
            std::uint64_t ordinal{};
            for (const auto& error : creation_value->failures())
                emit(error.canonical(), *error.digest(), error_serialization::profile, ordinal++,
                     error.provenance());
        } else if (r.event_facts)
            emit(r.canonical, r.digest, journal_profile(r.version), 0);
        else if (const auto* outcome_value =
                     std::get_if<source_outcome_serialization::Owner>(&r.artifact))
            emit(outcome_value->canonical(), *outcome_value->digest(),
                 source_outcome_serialization::profile, 0);
    }
}
auto pair_row(Node rows, std::size_t ordinal) -> Node {
    auto row = rows.element();
    row.object();
    row.decimal("pair_id", ordinal);
    return row.child("value");
}
void available(Node n) {
    n.object();
    n.null("absent_reason");
}
void unavailable(Node n, SV reason) {
    n.object();
    n.null("value");
    n.text("absent_reason", reason);
}
auto child_record(const ep::Transaction& tx, const ep::detail::ComposedPair& p, std::uint64_t child)
    -> const ep::Record& {
    require(child && child <= p.children.size(), Error::reference);
    const auto sequence = p.children[static_cast<std::size_t>(child - 1)];
    require(sequence && sequence <= tx.records().size(), Error::reference);
    return tx.records()[static_cast<std::size_t>(sequence - 1)];
}
void event_component(Node n, const ep::Record& record, SV bytes, Budget& b) {
    n.object();
    reference(n.child("source"), record);
    n.text("handoff_utc", record.timestamp);
    auto body = parse(bytes, b);
    n.copy("component", body.value);
}
// Closed v3 presentation reference. Its target is the already admitted component
// string in this document's embedded event table; full regeneration checks every
// source/pointer and never turns decoded evidence into a producer capability.
void component_reference(Node n, const ep::Record& source, SV pointer) {
    n.object();
    n.text("representation", "embedded_event_component");
    reference(n.child("source"), source);
    n.text("pointer", pointer);
}
void facts_reference(Node n, const ep::Record& source, SV pointer) {
    const bool whole = pointer.empty() &&
                       (source.kind == ep::Kind::placement ||
                        source.kind == ep::Kind::preparation || source.kind == ep::Kind::preflight);
    const bool timing = pointer == "/timing" &&
                        (source.kind == ep::Kind::observation ||
                         source.kind == ep::Kind::failed_observation);
    const bool checks = pointer == "/checks" && source.kind == ep::Kind::preflight;
    require(source.version == ep::Version::compact_v3 && source.event_facts &&
                (whole || timing || checks), Error::reference);
    n.object();
    n.text("representation", "embedded_event_facts");
    reference(n.child("source"), source);
    n.text("pointer", pointer);
}
void analysis_reference(Node n, const ep::Record& source, SV pointer) {
    require(source.version == ep::Version::compact_v3 && source.kind == ep::Kind::analysis &&
                source.event_facts &&
                (pointer == "/population" || pointer == "/analysis" || pointer == "/policies"),
            Error::reference);
    n.object();
    n.text("representation", "embedded_event_analysis");
    reference(n.child("source"), source);
    n.text("pointer", pointer);
}
auto copy_block(std::span<const block_evidence::Event> rows)
    -> runtime::PairwiseBlockAttemptEvidence {
    require(rows.size() == 6 && rows.front().kind == block_evidence::EventKind::opened &&
                rows.back().kind == block_evidence::EventKind::closed &&
                rows.back().completion == runtime::BlockAttemptCompletion::complete_valid &&
                rows.front().component.plan(),
            Error::association);
    std::vector<runtime::FactualObservationEvidence> facts;
    facts.reserve(4);
    for (std::size_t i = 1; i < 5; ++i) {
        require(rows[i].kind == block_evidence::EventKind::factual && rows[i].component.factual(),
                Error::association);
        facts.push_back(*rows[i].component.factual());
    }
    return {*rows.front().component.plan(),
            std::move(facts),
            {},
            runtime::BlockAttemptCompletion::complete_valid};
}
// The returned value is a presentation tree only; the immutable source prefix is
// never consumed, including allocation failure during the ordinary owning copy.
auto statistics_projection(const ep::detail::ComposedPair& pair, Budget& b) -> Tree {
    Tree out;
    Node n{out.value, b, 1};
    b.cell(1);
    n.object();
    if (pair.analysis) {
        auto raw = parse(pair.analysis_canonical, b);
        n.text("scope", "derived_analysis");
        n.present("result").copy(raw.value);
        return out;
    }
    n.text("scope", "diagnostic_partial");
    const auto stock = math_stock(pair);
    const auto serializer = add(mul(b.limits.analysis.output_bytes, 4U),
                                mul(b.limits.analysis.nodes, sizeof(J) * 4U));
    const auto serialization_live = stock.blocks ? add(add(stock.copied, stock.retained), serializer) : 0U;
    // Both future phases are admitted before the first immutable block copy.
    b.peak(std::max(stock.peak, serialization_live));
    b.work(stock.work);
    if (!stock.blocks) {
        n.absent("result", "no_complete_valid_blocks");
        return out;
    }
    require(pair.prefix && pair.context, Error::association);
    std::vector<runtime::PairwiseBlockAttemptEvidence> blocks;
    blocks.reserve(stock.blocks);
    for (std::size_t i{}; i < pair.prefix->block_count(); ++i) {
        const auto rows = pair.prefix->events(i);
        if (!rows.empty() && rows.back().kind == block_evidence::EventKind::closed &&
            rows.back().completion == runtime::BlockAttemptCompletion::complete_valid)
            blocks.push_back(copy_block(rows));
    }
    auto calculated =
        detail::calculate_pairwise_statistics(blocks, pair.context->header.statistics_policy);
    if (!calculated) {
        require(calculated.error().kind == detail::CalculationFailureKind::numerical,
                Error::calculation);
        n.absent("result", "numerical_failure");
        auto failure = n.object("failure");
        failure.text("stage", calculated.error().fact.stage_name());
        failure.text("code", calculated.error().fact.stable_code());
        return out;
    }
    struct RestoreBase {
        Budget& budget;
        std::size_t previous;
        ~RestoreBase() { budget.base = previous; }
    } restore{b, b.base};
    b.base = add(b.base, serialization_live);
    b.peak(0);
    auto serialized = analysis::serialize_diagnostic_statistics(
        *calculated, blocks, pair.context->header.statistics_policy, b.limits.analysis);
    core::detail::ExpectedErrorCleanup cleanup{serialized};
    require(serialized.has_value(), Error::calculation);
    auto body = parse(*serialized, b);
    n.present("result").copy(body.value);
    return out;
}
void metric_projection(Node n, const runtime::FactualObservationEvidence* fact) {
    if (!fact) {
        unavailable(n, "not_observed");
        return;
    }
    auto emit = [&](const auto& observed) {
        using T = std::decay_t<decltype(observed)>;
        constexpr bool bandwidth =
            std::is_same_v<T, measurement::SequentialReadBandwidthObservation>;
        auto result = [&] {
            if constexpr (bandwidth)
                return statistics::Bandwidth::create(observed.aggregate_work(),
                                                     observed.enclosing_interval().elapsed());
            else
                return statistics::DependentLoadLatency::create(
                    observed.enclosing_interval().elapsed(), observed.aggregate_work());
        }();
        core::detail::ExpectedErrorCleanup cleanup{result};
        if (!result) {
            unavailable(n, "numerical_failure");
            return;
        }
        available(n);
        auto value = n.object("value");
        const auto scalar = [&] {
            if constexpr (bandwidth)
                return result->bytes_per_second();
            else
                return result->seconds_per_completed_load();
        }();
        value.child("base").value = scalar;
        value.child("display").value = scalar;
        value.text("unit", bandwidth ? "bytes_per_second" : "seconds_per_completed_load");
        value.text("better", bandwidth ? "higher" : "lower");
        value.text("orientation",
                   bandwidth ? "candidate_over_baseline" : "baseline_over_candidate");
        value.text("formula", bandwidth ? "logical_bytes/elapsed_seconds"
                                        : "elapsed_seconds/completed_dependent_loads");
    };
    std::visit(emit, fact->observation);
}
template <class Metric> auto metric_scalar(Metric value) -> double {
    if constexpr (std::is_same_v<Metric, statistics::Bandwidth>)
        return value.bytes_per_second();
    else
        return value.seconds_per_completed_load();
}
void representatives(Node n, std::span<const block_evidence::Event> rows, bool eligible) {
    if (!eligible) {
        n.absent("representatives", "not_applicable");
        n.absent("effect", "not_applicable");
        return;
    }
    require(rows.size() == 6 && rows[1].component.factual(), Error::association);
    std::visit(
        [&](const auto& first) {
            using Observation = std::decay_t<decltype(first)>;
            constexpr bool bandwidth =
                std::is_same_v<Observation, measurement::SequentialReadBandwidthObservation>;
            using Metric = std::conditional_t<bandwidth, statistics::Bandwidth,
                                              statistics::DependentLoadLatency>;
            std::array<std::optional<Metric>, 2> a{}, z{};
            std::size_t ac{}, zc{};
            bool failed{};
            for (std::size_t i = 1; i < 5; ++i) {
                const auto* fact = rows[i].component.factual();
                require(fact, Error::association);
                const auto* value = std::get_if<Observation>(&fact->observation);
                require(value, Error::association);
                auto metric = [&] {
                    if constexpr (bandwidth)
                        return Metric::create(value->aggregate_work(),
                                              value->enclosing_interval().elapsed());
                    else
                        return Metric::create(value->enclosing_interval().elapsed(),
                                              value->aggregate_work());
                }();
                core::detail::ExpectedErrorCleanup cleanup{metric};
                if (!metric) {
                    failed = true;
                    break;
                }
                const bool baseline =
                    fact->slot.comparison_variant() == measurement::ComparisonVariantSlot::first;
                auto& count = baseline ? ac : zc;
                require(count < 2, Error::association);
                (baseline ? a : z)[count++] = *metric;
            }
            if (failed) {
                n.absent("representatives", "numerical_failure");
                n.absent("effect", "numerical_failure");
                return;
            }
            require(ac == 2 && zc == 2, Error::association);
            auto reps = [&] {
                if constexpr (bandwidth)
                    return statistics::block_bandwidth_representatives({*a[0], *a[1]},
                                                                       {*z[0], *z[1]});
                else
                    return statistics::block_dependent_load_latency_representatives({*a[0], *a[1]},
                                                                                    {*z[0], *z[1]});
            }();
            core::detail::ExpectedErrorCleanup cleanup{reps};
            if (!reps) {
                n.absent("representatives", "numerical_failure");
                n.absent("effect", "numerical_failure");
                return;
            }
            auto output = n.present("representatives");
            output.object();
            output.child("baseline").value = metric_scalar(reps->first());
            output.child("candidate").value = metric_scalar(reps->second());
            output.text("method", "arithmetic_mean");
            auto effect = [&] {
                if constexpr (bandwidth)
                    return statistics::bandwidth_log_effect(*reps);
                else
                    return statistics::dependent_load_latency_log_effect(*reps);
            }();
            core::detail::ExpectedErrorCleanup effect_cleanup{effect};
            if (!effect)
                n.absent("effect", "numerical_failure");
            else
                n.present("effect").value = effect->value();
        },
        rows[1].component.factual()->observation);
}
void outlier(Node n, const J& stats, std::uint64_t id) {
    const auto& result = stats.at("result").at("value");
    const J* values{};
    if (!result.is_null()) {
        if (core::detail::json_string_equal(stats.at("scope"), "diagnostic_partial"))
            values = &result.at("diagnostic");
        else if (!result.at("analysis").at("value").is_null())
            values = &result.at("analysis").at("value");
    }
    if (values)
        for (const auto role : {"first", "second"}) {
            const auto& variant = values->at(role);
            const auto& ids = variant.at("observation_ids");
            n.budget.work(ids.size());
            for (std::size_t i{}; i < ids.size(); ++i)
                if (decimal(ids[i]) == id) {
                    n.present("outlier").copy(variant.at("outliers").at("flags").at(i));
                    return;
                }
        }
    n.absent("outlier", "not_applicable");
}
void capture_projection(Node n, SV run, const ep::Record& source, const J& capture,
                        std::size_t ordinal, const J& association, Budget& b) {
    n.object();
    n.decimal("pair_id", ordinal);
    reference(n.child("source"), source, "monitoring");
    const bool compact = source.version == ep::Version::compact_v3;
    const SV pointer = source.kind == ep::Kind::monitoring ? SV{} : SV{"/monitoring"};
    if (compact)
        component_reference(n.child("capture"), source, pointer);
    else
        n.copy("capture", capture);
    n.copy("observation_ref", association);
    auto samples = compact ? n.object("samples") : n.array("samples");
    if (compact)
        samples.text("representation", "embedded_capture_samples");
    std::size_t index{};
    const auto& raw = capture.at("raw");
    const auto append = [&](SV kind, const J& value, std::optional<std::size_t> group) {
        b.usage.samples = add(b.usage.samples, 1);
        require(b.usage.samples <= b.limits.samples, Error::capacity);
        if (compact) {
            // Every raw acquisition remains in the single referenced capture.
            // Charge its count even though v3 does not duplicate a sample table.
            b.work(1);
            ++index;
            return;
        }
        auto sample = samples.element();
        sample.object();
        sample.text("id", std::string{run} + ":" + std::to_string(source.sequence) +
                              ":sample:" + std::to_string(index));
        reference(sample.child("source"), source, "sample", index++);
        sample.text("kind", kind);
        if (group)
            sample.decimal("group", *group);
        else
            sample.null("group");
        sample.copy("value", value);
    };
    for (const auto key :
         {"page_fault_before", "page_fault_after", "context_switches", "temperature"})
        for (const auto& value : raw.at(key))
            append(key, value, {});
    std::size_t group{};
    for (const auto& frequency : raw.at("frequency")) {
        for (const auto& value : frequency.at("acquisitions"))
            append("frequency", value, group);
        ++group;
    }
    if (compact)
        samples.decimal("count", index);
}
void compact_capture_projection(Node n, const ep::Record& source,
                                const monitoring::MonitoringCaptureResult& capture,
                                std::size_t ordinal, const J& association, Budget& b) {
    n.object();
    n.decimal("pair_id", ordinal);
    reference(n.child("source"), source, "monitoring");
    component_reference(n.child("capture"), source, "/monitoring");
    n.copy("observation_ref", association);
    auto samples = n.object("samples");
    samples.text("representation", "embedded_capture_samples");
    const auto& raw = capture.raw();
    auto count = add(add(raw.page_fault_before().size(), raw.page_fault_after().size()),
                     add(raw.context_switches().size(), raw.temperature().size()));
    for (const auto& frequency : raw.frequency()) {
        b.work(1);
        count = add(count, frequency.acquisitions.size());
    }
    b.usage.samples = add(b.usage.samples, count);
    require(b.usage.samples <= b.limits.samples, Error::capacity);
    b.work(count);
    samples.decimal("count", count);
}
void experiments(Node n, const Sources& s, const ep::Transaction& tx, Budget& b) {
    const auto* plan = s.composition ? s.composition->planning.get() : nullptr;
    const auto total_pairs = plan ? plan->pairs.size() : 0U;
    require(total_pairs <= b.limits.pairs, Error::capacity);
    b.usage.pairs = std::max(b.usage.pairs, total_pairs);
    n.array();
    const auto* retained = s.normalized ? s.normalized->plan() : nullptr;
    if (!retained)
        return;
    auto resolved = trusted_plan_tree(retained->canonical_resolved_plan(), b);
    auto semantic = trusted_plan_tree(retained->canonical_scenario(), b);
    normalized_numbers(resolved.value, b);
    normalized_numbers(semantic.value, b);
    const auto& inputs = semantic.value.at("experiments");
    const auto& planned = resolved.value.at("experiments");
    require(inputs.is_array() && planned.is_array() && inputs.size() == planned.size(),
            Error::association);
    require(inputs.size() <= b.limits.experiments, Error::capacity);
    b.usage.experiments = std::max(b.usage.experiments, inputs.size());
    // Admit every pair policy before any raw-block copy or calculation.
    preflight_math(tx, b);
    for (std::size_t experiment{}; experiment < inputs.size(); ++experiment) {
        const auto& intent = inputs[experiment];
        const auto& resolved_experiment = planned[experiment];
        auto row = n.element();
        row.object();
        row.copy("experiment_id", intent.at("id"));
        row.copy("type", intent.at("type"));
        auto status = row.object("status");
        status.text("execution", "planned");
        status.text("validity", "not_evaluated");
        status.text("conclusion", "not_evaluated");
        auto workload = row.object("workload");
        // The named complete dispatch artifact retains every configuration value;
        // selection below adds only the separately admitted implementation join.
        artifact(workload, "dispatch", retained->kernel_dispatch(),
                 kernel_dispatch_serialization::profile);
        if (plan) {
            const bool plugins=implementation_selection::detail::uses_plugins(plan->workloads);
            require(!plugins||tx.version()==ep::Version::compact_v3,Error::schema);
            auto selected = workload.present("selection");
            selected.array();
            for (const auto& binding : plan->bindings)
                if (binding.experiment_index == experiment) {
                    require(binding.workload_index < plan->workloads.size(), Error::association);
                    const auto& value = plan->workloads[binding.workload_index];
                    auto item = selected.element();
                    item.object();
                    item.text("workload_id", value.workload_id);
                    if(plugins) {
                        item.text("schema","epycbench.kernel-selection/v2");
                        item.text("semantic",kernel::kernel_id_token(value.semantic));
                        item.text("implementation",kernel::kernel_implementation_id_token(value.implementation));
                        item.text("family",value.semantic.family()==kernel::KernelFamily::bandwidth?"bandwidth":"latency");
                        if(value.module) {
                            auto module=item.object("module");
                            module.text("version",value.module->version);module.text("build_info",value.module->build_info);
                            module.text("file_sha256",value.module->file_sha256);
                        } else item.null("module");
                    } else {
                        item.number("semantic", value.semantic==kernel::KernelId::sequential_read_bandwidth_v1?0U:1U);
                        item.number("implementation", static_cast<unsigned>(value.implementation));
                    }
                }
        } else
            workload.absent("selection", "plan_unavailable");
        auto placement = row.object("placement");
        placement.copy("resolved", resolved_experiment);
        artifact(placement, "resources", retained->resource_service(),
                 resource_service_serialization::profile);
        artifact(placement, "memory", retained->memory_placement(),
                 memory_placement_serialization::profile);
        auto actual = placement.array("actual");
        auto calibration = row.array("calibration"), warmup = row.array("warmup"),
             blocks = row.array("blocks"), observations = row.array("observations"),
             monitoring = row.array("monitoring_samples"), checks = row.array("validation_events"),
             stats = row.array("statistics"), comparison = row.array("comparison"),
             errors = row.array("diagnostics");
        if (s.composition && s.composition->preflight_record) {
            const auto& pre =
                tx.records()[static_cast<std::size_t>(s.composition->preflight_record - 1)];
            auto item = checks.element();
            item.object();
            reference(item.child("source"), pre);
            item.text("scope", "preflight");
            if (s.compact)
                facts_reference(item.child("checks"), pre, "/checks");
            else
                item.copy("checks", pre.event_facts->at("checks"));
        }
        if (!plan)
            continue;
        std::size_t pair_count{}, completed{}, invalid{}, valid{};
        for (const auto& declared : plan->pairs) {
            if (declared.experiment_index != experiment)
                continue;
            b.work(1);
            require(++pair_count <= b.limits.pairs, Error::capacity);
            b.usage.pairs = std::max(b.usage.pairs, plan->pairs.size());
            const auto ordinal = declared.ordinal;
            require(ordinal < s.composition->pairs.size(), Error::association);
            const auto* pair = s.composition->pairs[ordinal].get();
            if (!pair) {
                for (auto target : {actual, calibration, warmup, stats, comparison})
                    unavailable(pair_row(target, ordinal), "not_started");
                continue;
            }
            status.value.at("execution") = "running";
            auto actual_value = pair_row(actual, ordinal);
            available(actual_value);
            auto phases = actual_value.array("value");
            for (auto sequence : pair->events) {
                const auto& event = tx.records()[static_cast<std::size_t>(sequence - 1)];
                if (event.kind == ep::Kind::placement || event.kind == ep::Kind::preparation) {
                    auto phase = phases.element();
                    phase.object();
                    reference(phase.child("source"), event);
                    phase.text("handoff_utc", event.timestamp);
                    if (s.compact)
                        facts_reference(phase.child("facts"), event, "");
                    else
                        phase.copy("facts", *event.event_facts);
                }
            }
            const auto* prefix = pair->prefix ? &*pair->prefix : nullptr;
            const auto emit_outcome = [&](Node target, const auto* outcome) {
                auto output = pair_row(target, ordinal);
                if (!outcome) {
                    unavailable(output, "not_observed");
                    return;
                }
                available(output);
                const auto& original = child_record(tx, *pair, outcome->sequence);
                if (s.compact) {
                    auto value = output.object("value");
                    reference(value.child("source"), original);
                    value.text("handoff_utc", original.timestamp);
                    component_reference(value.child("component"), original, "");
                } else
                    event_component(output.child("value"), original, outcome->canonical, b);
            };
            emit_outcome(calibration, prefix ? prefix->calibration_event() : nullptr);
            emit_outcome(warmup, prefix ? prefix->warmup_event() : nullptr);
            const ep::Record* analysis_source{};
            if (s.compact && pair->analysis) {
                require(pair->analysis_record && pair->analysis_record <= tx.records().size(),
                        Error::reference);
                analysis_source = &tx.records()[static_cast<std::size_t>(pair->analysis_record - 1)];
            }
            auto stat_tree = statistics_projection(*pair, b);
            if (analysis_source) {
                auto projected = pair_row(stats, ordinal);
                projected.object();
                projected.copy("scope", stat_tree.value.at("scope"));
                auto result = projected.present("result");
                result.object();
                const auto& body = stat_tree.value.at("result").at("value");
                for (auto field = body.begin(); field != body.end(); ++field)
                    if (field.key() == "population")
                        analysis_reference(result.child("population"), *analysis_source, "/population");
                    else
                        result.copy(field.key(), field.value());
            } else
                pair_row(stats, ordinal).copy(stat_tree.value);
            auto compare = pair_row(comparison, ordinal);
            compare.object();
            compare.number("baseline_index", declared.baseline_index);
            compare.number("candidate_index", declared.candidate_index);
            compare.text("conclusion", "not_evaluated");
            if (pair->analysis) {
                auto analysis_body = parse(pair->analysis_canonical, b);
                if (analysis_source) {
                    analysis_reference(compare.child("analysis"), *analysis_source, "/analysis");
                    analysis_reference(compare.present("policies"), *analysis_source, "/policies");
                } else {
                    compare.copy("analysis", analysis_body.value.at("analysis"));
                    compare.present("policies").copy(analysis_body.value.at("policies"));
                }
                if (pair->analysis->validity() == PairwiseResultValidity::invalid)
                    ++invalid;
                if (pair->analysis->validity() == PairwiseResultValidity::valid) {
                    ++valid;
                    if (pair->analysis->authoritative_statistics() && pair->cleanup_complete &&
                        pair->clean_eof && pair->reap_record && s.composition->closed_record)
                        compare.value.at("conclusion") =
                            string(analysis_body.value.at("analysis").at("value").at("conclusion"));
                }
                if (pair->cleanup_complete && pair->clean_eof && pair->reap_record)
                    ++completed;
            } else {
                compare.absent("analysis", "not_observed");
                compare.absent("policies", pair->context ? "not_observed" : "not_started");
            }
            b.work(tx.records().size());
            for (const auto& event : tx.records()) {
                if (event.scope.pair == ordinal && event.scope.experiment == experiment &&
                    (event.kind == ep::Kind::failure ||
                     event.kind == ep::Kind::failed_observation)) {
                    auto item = errors.element();
                    item.object();
                    item.decimal("pair_id", ordinal);
                    reference(item.child("source"), event);
                    item.copy("facts", *event.event_facts);
                }
            }
            if (!prefix)
                continue;
            for (std::size_t bi{}; bi < prefix->block_count(); ++bi) {
                const auto events = prefix->events(bi);
                require(!events.empty() && events.front().component.plan(), Error::association);
                b.usage.blocks = add(b.usage.blocks, 1);
                require(b.usage.blocks <= b.limits.blocks, Error::capacity);
                const auto& opened = child_record(tx, *pair, events.front().sequence);
                auto block = blocks.element();
                block.object();
                block.decimal("pair_id", ordinal);
                block.decimal("block_id", events.front().component.plan()->block_id().value());
                reference(block.child("opened"), opened);
                auto block_plan = parse(events.front().component.canonical(), b);
                block.copy("plan", block_plan.value);
                const bool closed = events.back().kind == block_evidence::EventKind::closed;
                const bool eligible = closed && events.back().completion ==
                                                    runtime::BlockAttemptCompletion::complete_valid;
                if (closed) {
                    const auto& close = child_record(tx, *pair, events.back().sequence);
                    reference(block.present("closed"), close);
                    block.present("completion").copy(close.event_facts->at("completion"));
                } else {
                    block.absent("closed", "not_observed");
                    block.absent("completion", "not_observed");
                }
                block.boolean("included", eligible);
                block.text("reason",
                           eligible ? "complete_valid" : "paired_block_incomplete_or_invalid");
                representatives(block, events, eligible);
                auto observed = block.array("observations");
                for (const auto& event : events) {
                    if (event.kind != block_evidence::EventKind::factual &&
                        event.kind != block_evidence::EventKind::failed)
                        continue;
                    b.usage.observations = add(b.usage.observations, 1);
                    require(b.usage.observations <= b.limits.observations, Error::capacity);
                    const auto& original = child_record(tx, *pair, event.sequence);
                    reference(observed.element(), original);
                    auto item = observations.element();
                    item.object();
                    item.decimal("pair_id", ordinal);
                    item.copy("block_id", block.value.at("block_id"));
                    item.decimal("observation_id",
                                 event.component.factual()
                                     ? event.component.factual()->observation_id.value()
                                     : event.component.failed()->observation_id.value());
                    reference(item.child("source"), original);
                    auto raw = s.compact && event.component.factual()
                                   ? compact_factual_tree(event.component, b)
                                   : parse(event.component.canonical(), b);
                    // Preserve export-consumed measurement/placement/disposition
                    // fields. Duplicated raw Monitoring and validity evidence
                    // remain in the original, unmodified event component.
                    if (original.version == ep::Version::compact_v3 &&
                        event.component.factual()) {
                        Tree projected;
                        Node component{projected.value, b, 1};
                        b.cell(1);
                        component.object();
                        for (auto field = raw.value.begin(); field != raw.value.end(); ++field)
                            if (field.key() == "monitoring" &&
                                event.component.factual()->monitoring.has_value())
                                component_reference(component.child("monitoring"), original,
                                                    "/monitoring");
                            else if (field.key() == "validity") {
                                auto validity = component.object("validity");
                                validity.copy("disposition", field.value().at("disposition"));
                                component_reference(validity.child("evidence"), original,
                                                    "/validity");
                            }
                            else
                                component.copy(field.key(), field.value());
                        item.copy("component", projected.value);
                    } else {
                        item.copy("component", raw.value);
                    }
                    if (s.compact)
                        facts_reference(item.child("timing"), original, "/timing");
                    else
                        item.copy("timing", original.event_facts->at("timing"));
                    item.copy("monitoring_refs", original.event_facts->at("monitoring_refs"));
                    metric_projection(item.child("metric"), event.component.factual());
                    item.boolean("included", eligible);
                    item.text("reason",
                              eligible ? "complete_valid" : "paired_block_incomplete_or_invalid");
                    item.absent("worker_tid", "not_retained");
                    outlier(item, stat_tree.value,
                            event.component.factual()
                                ? event.component.factual()->observation_id.value()
                                : event.component.failed()->observation_id.value());
                    if (event.component.factual()) {
                        if (s.compact)
                            component_reference(item.present("workers"), original,
                                                "/measurement/workers");
                        else
                            item.present("workers").copy(raw.value.at("measurement").at("workers"));
                    } else
                        item.absent("workers", "not_retained");
                    if (event.component.factual()) {
                        auto validation = checks.element();
                        validation.object();
                        reference(validation.child("source"), original);
                        validation.text("scope", "observation");
                        if (original.version == ep::Version::compact_v3)
                            component_reference(validation.child("checks"), original, "/validity");
                        else
                            validation.copy("checks", raw.value.at("validity"));
                        const auto& links = original.event_facts->at("monitoring_refs");
                        if (links.size() == 1 && decimal(links[0].at("sequence")) == original.sequence) {
                            Tree association;
                            Node link{association.value, b, 1};
                            reference(link, original, "observation");
                            if (s.compact) {
                                const auto& capture = event.component.factual()->monitoring;
                                if (capture)
                                    compact_capture_projection(monitoring.element(), original,
                                                               *capture, ordinal, association.value, b);
                            } else {
                                const auto& capture = raw.value.at("monitoring");
                                if (!capture.is_null())
                                    capture_projection(monitoring.element(), s.genesis->provenance()->run,
                                                       original, capture, ordinal, association.value, b);
                            }
                        }
                    }
                }
            }
            for (auto sequence : pair->events) {
                const auto& event = tx.records()[static_cast<std::size_t>(sequence - 1)];
                if (event.kind == ep::Kind::monitoring) {
                    auto capture = parse(string(event.event_facts->at("component")), b);
                    capture_projection(monitoring.element(), s.genesis->provenance()->run, event,
                                       capture.value, ordinal,
                                       event.event_facts->at("observation_ref"), b);
                }
            }
        }
        if ((pair_count && completed == pair_count) ||
            (!pair_count && plan && s.composition->closed_record &&
             core::detail::json_string_equal(
                 tx.records()[static_cast<std::size_t>(s.composition->closed_record - 1)]
                     .event_facts->at("execution"),
                 "completed")))
            status.value.at("execution") = "completed";
        if (s.composition->closed_record) {
            const auto& closed =
                *tx.records()[static_cast<std::size_t>(s.composition->closed_record - 1)]
                     .event_facts;
            for (const auto& declared : plan->pairs)
                if (declared.experiment_index == experiment &&
                    core::detail::json_string_equal(
                        closed.at("pairs").at(declared.ordinal).at("state"), "failed"))
                    status.value.at("execution") = "failed";
        }
        if (invalid) {
            status.value.at("validity") = "invalid";
            status.value.at("conclusion") = "invalid";
        } else if (pair_count && valid == pair_count && completed == pair_count)
            status.value.at("validity") = "valid";
        if (!invalid && pair_count == 1 && comparison.value.size() == 1)
            status.value.at("conclusion") =
                comparison.value[0].at("value").value("conclusion", std::string{"not_evaluated"});
    }
}
auto project(const ep::Transaction& tx, const fs::Receipt& receipt, const FinalizationFacts& final,
             Budget& b) -> Tree {
    const auto source = sources(tx);
    std::size_t frames{};
    for (const auto& r : tx.records())
        frames = add(frames, add(r.canonical.size(), 2));
    check_receipt(receipt, tx, frames, b);
    Tree tree;
    Node root{tree.value, b, 1};
    b.cell(1);
    root.object();
    auto schema = root.object("schema");
    schema.text("name", "epycbench.result");
    schema.number("major", 1);
    schema.number("revision", document_revision(tx.version()));
    schema.text("json_schema_dialect", "https://json-schema.org/draft/2020-12/schema");
    auto id = root.object("identity");
    id.text("run_id", source.genesis->provenance()->run);
    if (source.normalized) {
        const auto* plan = source.normalized->plan();
        auto scenario = trusted_plan_tree(plan->canonical_scenario(), b);
        id.present("scenario_id").copy(scenario.value.at("scenario_id"));
        id.present("scenario_digest").text(plan->scenario_digest()->text());
        id.present("resolved_scenario_plan_digest").text(plan->resolved_digest()->text());
    } else
        for (auto key : {"scenario_id", "scenario_digest", "resolved_scenario_plan_digest"})
            id.absent(key, "not_obtained");
    if (source.composition && source.composition->planning)
        id.present("execution_plan_digest")
            .text(source.composition->planning->execution_digest.to_hex());
    else
        id.absent("execution_plan_digest", "plan_unavailable");
    if (source.topology && source.topology->topology()) {
        const auto* facts = source.topology->topology()->fingerprints();
        id.present("physical_topology_fingerprint").text(facts->physical.to_string());
        id.present("effective_topology_fingerprint").text(facts->effective.to_string());
    } else {
        id.absent("physical_topology_fingerprint", "not_obtained");
        id.absent("effective_topology_fingerprint", "not_obtained");
    }
    const auto* base = source.genesis->base_build();
    if (base && base->manifest())
        id.present("build_digest").text(base->manifest()->digest()->value().to_hex());
    else
        id.absent("build_digest", "not_obtained");
    const auto status = projected_status(tx, receipt);
    auto state = root.object("status");
    constexpr std::array<SV, 5> keys{"execution", "validity", "conclusion", "observed_execution",
                                     "recovery_disposition"};
    for (std::size_t i{}; i < keys.size(); ++i)
        state.text(keys[i], status[i]);
    auto integrity = root.object("integrity");
    integrity.text("canonicalization", "RFC8785");
    integrity.text("decimal_profile", "epycbench.decimal-string/v1");
    integrity.text("hash", "SHA256");
    integrity.null("content_digest");
    integrity.decimal("journal_record_count", tx.summary().records);
    if (tx.summary().digest)
        integrity.text("journal_final_digest", tx.summary().digest->to_hex());
    else
        integrity.null("journal_final_digest");
    integrity.text("verification", "contained_facts_verified");
    source_projection(root.object("scenario"), source, b);
    plan_projection(root.object("plan"), source);
    auto build = root.object("build");
    artifact(build, "base", base, base_build_serialization::profile);
    if (source.compact && source.composition && source.composition->planning) {
        require(source.planning_record, Error::reference);
        artifact_reference(build.present("object_evidence"), *source.planning_record,
                           ArtifactSelector::planning_object_evidence,
                           execution_plan::artifact_profile, source.composition->planning->digest);
    } else if (source.composition && source.composition->planning) {
        auto evidence = build.present("object_evidence");
        evidence.array();
        for (const auto& approval : source.composition->planning->approvals) {
            auto row = evidence.element();
            row.object();
            if (approval) {
                row.null("absent_reason");
                auto value = row.object("value");
                value.text("manifest_bytes_hex", hex(approval->bytes(), b));
                value.text("digest", approval->digest()->to_hex());
                auto reports = value.array("reports");
                for (std::size_t i{}; i < 2; ++i) {
                    auto report = reports.element();
                    report.object();
                    const auto* supplied = approval->report(i);
                    require(supplied && supplied->digest(), Error::association);
                    report.text("machine_bytes_hex", hex(supplied->bytes(), b));
                    report.text("machine_digest", supplied->digest()->to_hex());
                    report.text("human_bytes_hex", hex(approval->human_bytes(i), b));
                }
            } else {
                row.null("value");
                row.text("absent_reason", "not_obtained");
            }
        }
    } else
        build.absent("object_evidence", "plan_unavailable");
    build.copy("manifest_digest", tree.value.at("identity").at("build_digest"));
    auto host = root.object("host");
    if (source.compact && source.genesis->host()) {
        require(source.genesis_record && source.genesis->host()->digest(), Error::reference);
        artifact_reference(host.present("artifact"), *source.genesis_record,
                           ArtifactSelector::creation_host, host_serialization::profile,
                           *source.genesis->host()->digest());
    } else {
        artifact(host, "artifact", source.genesis->host(), host_serialization::profile);
    }
    auto topology = root.object("topology");
    if (source.compact && source.topology) {
        require(source.topology_record && source.topology->digest(), Error::reference);
        artifact_reference(topology.present("artifact"), *source.topology_record,
                           source.topology_selector, topology_serialization::profile,
                           *source.topology->digest());
    } else {
        artifact(topology, "artifact", source.topology, topology_serialization::profile);
    }
    topology.copy("physical_fingerprint",
                  tree.value.at("identity").at("physical_topology_fingerprint"));
    topology.copy("effective_fingerprint",
                  tree.value.at("identity").at("effective_topology_fingerprint"));
    auto preflight = root.object("preflight");
    if (source.composition && source.composition->preflight_record) {
        const auto& record =
            tx.records()[static_cast<std::size_t>(source.composition->preflight_record - 1U)];
        if (source.compact) {
            auto view = preflight.present("artifact");
            view.object();
            view.copy("origin", record.event_facts->at("origin"));
            facts_reference(view.child("evidence"), record, "");
        } else
            preflight.present("artifact").copy(*record.event_facts);
        constexpr std::array<std::pair<SV, SV>, 7> fields_map{{{"start", "started_at"},
                                                               {"end", "ended_at"},
                                                               {"status", "status"},
                                                               {"checks", "checks"},
                                                               {"system_state", "references"},
                                                               {"warnings", "warnings"},
                                                               {"overrides", "overrides"}}};
        for (auto [key, field] : fields_map)
            if (source.compact && key == "checks")
                facts_reference(preflight.present(key), record, "/checks");
            else
                preflight.present(key).copy(record.event_facts->at(field));
    } else
        for (auto key : {"artifact", "start", "end", "status", "checks", "system_state", "warnings",
                         "overrides"})
            preflight.absent(key, "not_observed");
    experiments(root.child("experiments"), source, tx, b);
    diagnostics(root.array("run_diagnostics"), tx);
    auto journal = root.object("journal");
    journal.text("profile", journal_profile(tx.version()));
    auto records = journal.array("records");
    for (const auto& r : tx.records()) {
        if (tx.version() == ep::Version::compact_v3) {
            // The admitted record keeps its exact canonical bytes and digest.
            // Move its bounded object into the document rather than escaping a
            // second string copy. Parse usage includes its final nesting depth.
            auto record = parse(r.canonical, b, records.depth);
            require(record.value.is_object());
            require(render(record.value, b, b.limits.events.body_bytes) == r.canonical,
                    Error::canonical);
            records.value.push_back(std::move(record.value));
        } else {
            records.element().text(r.canonical);
        }
    }
    const auto summary = receipt.recovery ? *receipt.recovery : tx.summary();
    journal.decimal("record_count", summary.records);
    if (summary.digest)
        journal.text("final_digest", summary.digest->to_hex());
    else
        journal.null("final_digest");
    journal.text("tail", summary.tail == ep::Tail::clean_prefix      ? "clean_prefix"
                         : summary.tail == ep::Tail::incomplete_tail ? "incomplete_tail"
                                                                     : "corrupt");
    journal.decimal("fragment_bytes", summary.fragment_bytes);
    journal.text("recovery_disposition", status[4]);
    receipt_json(journal.present("filesystem"), receipt);
    auto finalized = root.object("finalization");
    finalized.text("id", final.id.text());
    finalized.text("timestamp", final.timestamp.text());
    finalized.text("origin", "supplied_synthetic");
    finalized.text("mode", status[0] == "completed" ? "normal" : "recovery");
    finalized.decimal("source_record_count", summary.records);
    if (summary.digest)
        finalized.text("source_digest", summary.digest->to_hex());
    else
        finalized.null("source_digest");
    if (source.composition && source.composition->closed_record)
        reference(finalized.present("observed_terminal"),
                  tx.records()[static_cast<std::size_t>(source.composition->closed_record - 1U)]);
    else
        finalized.absent("observed_terminal", "not_observed");
    if (receipt.failure)
        failure_json(finalized.present("recovery_failure"), *receipt.failure);
    else
        finalized.absent("recovery_failure", "not_observed");
    finalized.absent("publication", "not_observed");
    auto extension = root.object("extensions").object("epycbench.private_complete");
    extension.text("profile", complete_profile(tx.version()));
    bool observed{};
    if (ep::is_progress(tx.version())) for (const auto& record : tx.records()) {
        b.work(1);
        observed = observed || (record.availability &&
            record.availability->source != ep::AvailabilitySource::supplied_process_fact);
    }
    extension.text("origin", observed ? "mixed_observed_and_supplied" : "supplied_synthetic");
    return tree;
}
void initial(Budget& b, std::size_t input) {
    require(b.limits.depth && b.limits.depth <= 64 && input <= b.limits.input_bytes, Error::limits);
    require(b.limits.input_bytes <= std::string{}.max_size() &&
                b.limits.output_bytes <= std::string{}.max_size(),
            Error::limits);
    b.usage.input_bytes = input;
    // Whole-document strings/trees remain live while child parse/math/serialization
    // workspaces allocate. Reserve them persistently, never as a competing peak.
    const auto document = add(mul(b.limits.output_bytes, 4), mul(b.limits.nodes, sizeof(J) * 4U));
    b.base = add(add(input, b.limits.events.simultaneous_bytes), document);
    b.peak(0);
}
auto finish_digest(Tree& tree, Budget& b) -> std::string {
    const auto without = render(tree.value, b);
    const auto digest = core::sha256(without).to_hex();
    tree.value.at("integrity").at("content_digest") = digest;
    return render(tree.value, b);
}
auto assembly(const auto& input, const FinalizationFacts& f, Limits limits)
    -> std::expected<std::pair<std::string, Usage>, Failure> {
    try {
        const auto* tx = fs::detail::Access::transaction(input);
        require(tx, Error::state);
        Budget budget{limits, {}};
        initial(budget, tx->usage().canonical_bytes);
        auto tree = project(*tx, input.receipt(), f, budget);
        auto bytes = finish_digest(tree, budget);
        return std::pair{std::move(bytes), budget.usage};
    } catch (const Failure& e) {
        return std::unexpected{e};
    } catch (ep::Error e) {
        return std::unexpected{Failure{Error::association, e, {}, {}}};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{Failure{Error::capacity, {}, {}, {}}};
    } catch (const J::exception&) {
        return std::unexpected{Failure{Error::schema, {}, {}, {}}};
    }
}
} // namespace
struct Validated::Data {
    std::string canonical;
    core::Sha256Digest digest{core::sha256(SV{})};
    Usage usage;
    std::string leaf, temporary;
    bool publication_consumed{};
};
Validated::Validated(std::unique_ptr<Data> d) noexcept : data_(std::move(d)) {}
Validated::~Validated() = default;
Validated::Validated(Validated&&) noexcept = default;
auto Validated::operator=(Validated&&) noexcept -> Validated& = default;
auto Validated::canonical() const noexcept -> SV { return data_ ? SV{data_->canonical} : SV{}; }
auto Validated::digest() const noexcept -> const core::Sha256Digest* {
    return data_ ? &data_->digest : nullptr;
}
auto Validated::usage() const noexcept -> Usage { return data_ ? data_->usage : Usage{}; }
auto Validated::final_leaf() const noexcept -> SV { return data_ ? SV{data_->leaf} : SV{}; }
} // namespace epycbench::results::final_document

namespace epycbench::results::final_document {
auto assemble(const filesystem_journal::Writer& input, const FinalizationFacts& f, Limits limits)
    -> std::expected<Validated, Failure> {
    auto candidate = assembly(input, f, limits);
    if (!candidate)
        return std::unexpected{candidate.error()};
    // The assembly DOM has retired. Admit the validation phase against the
    // remaining cumulative visit/text/work stocks; candidate bytes are its input.
    const auto prior = candidate->second;
    const auto source_stock = fs::detail::Access::transaction(input)->usage().retained_bytes;
    if (source_stock > limits.simultaneous_bytes)
        return std::unexpected{Failure{Error::capacity, {}, {}, {}}};
    limits.simultaneous_bytes -= source_stock;
    limits.nodes -= prior.nodes;
    limits.text_bytes -= prior.text_bytes;
    limits.work -= prior.work;
    auto result = validate(candidate->first, limits);
    if (result) {
        auto& usage = result->data_->usage;
        usage.nodes += prior.nodes;
        usage.text_bytes += prior.text_bytes;
        usage.work += prior.work;
        usage.input_bytes = std::max(usage.input_bytes, prior.input_bytes);
        usage.simultaneous_bytes =
            std::max(usage.simultaneous_bytes + source_stock, prior.simultaneous_bytes);
    }
    return result;
}
auto assemble(const filesystem_journal::Recovery& input, const FinalizationFacts& f, Limits limits)
    -> std::expected<Validated, Failure> {
    auto candidate = assembly(input, f, limits);
    if (!candidate)
        return std::unexpected{candidate.error()};
    // The assembly DOM has retired. Admit the validation phase against the
    // remaining cumulative visit/text/work stocks; candidate bytes are its input.
    const auto prior = candidate->second;
    const auto source_stock = fs::detail::Access::transaction(input)->usage().retained_bytes;
    if (source_stock > limits.simultaneous_bytes)
        return std::unexpected{Failure{Error::capacity, {}, {}, {}}};
    limits.simultaneous_bytes -= source_stock;
    limits.nodes -= prior.nodes;
    limits.text_bytes -= prior.text_bytes;
    limits.work -= prior.work;
    auto result = validate(candidate->first, limits);
    if (result) {
        auto& usage = result->data_->usage;
        usage.nodes += prior.nodes;
        usage.text_bytes += prior.text_bytes;
        usage.work += prior.work;
        usage.input_bytes = std::max(usage.input_bytes, prior.input_bytes);
        usage.simultaneous_bytes =
            std::max(usage.simultaneous_bytes + source_stock, prior.simultaneous_bytes);
    }
    return result;
}
auto validate(std::string_view bytes, Limits limits) -> std::expected<Validated, Failure> {
    try {
        Budget b{limits, {}};
        initial(b, bytes.size());
        auto tree = parse(bytes, b);
        fields(tree.value, {"schema", "identity", "status", "integrity", "scenario", "plan",
                            "build", "host", "topology", "preflight", "experiments",
                            "run_diagnostics", "journal", "finalization", "extensions"});
        const auto version = document_version(tree.value);
        require(version == limits.events.version, Error::schema);
        require(render(tree.value, b) == bytes, Error::canonical);
        auto& integrity = tree.value.at("integrity");
        require(integrity.is_object() && integrity.contains("content_digest"));
        const auto digest = digest_value(integrity.at("content_digest"));
        require(digest.has_value(), Error::digest);
        integrity.at("content_digest") = nullptr;
        require(core::sha256(render(tree.value, b)) == *digest, Error::digest);
        integrity.at("content_digest") = digest->to_hex();
        const auto& id = tree.value.at("identity");
        require(id.is_object() && id.contains("run_id"));
        auto run = journal::RunId::parse(string(id.at("run_id")));
        core::detail::ExpectedErrorCleanup run_cleanup{run};
        require(run.has_value());
        // Reserve the unchanged child's complete finite work allowance before
        // its replay owner allocates. C4 scans and Statistics are additional work.
        b.work(limits.events.work);
        auto created = ep::Transaction::create(*run, limits.events);
        require(created.has_value(), Error::capacity);
        auto tx = std::move(*created);
        const auto& journal = tree.value.at("journal");
        require(journal.is_object() && journal.contains("records") &&
                journal.contains("filesystem"));
        const auto& records = journal.at("records");
        require(records.is_array() && records.size() <= limits.records, Error::capacity);
        for (const auto& row : records) {
            std::string canonical;
            SV text;
            if (version == ep::Version::compact_v3) {
                require(row.is_object());
                canonical = render(row, b, limits.events.body_bytes);
                text = canonical;
            } else {
                text = string(row);
            }
            preflight_math(tx, b);
            b.work(text.size());
            std::string frame;
            frame.reserve(add(text.size(), 2));
            frame.push_back('\x1e');
            frame += text;
            frame.push_back('\n');
            auto feed = tx.feed(frame);
            require(feed && feed->accepted && feed->consumed == frame.size(), Error::association);
        }
        auto completed = tx.finish();
        require(completed.has_value(), Error::association);
        const auto& slot = journal.at("filesystem");
        fields(slot, {"value", "absent_reason"});
        require(slot.at("absent_reason").is_null() && !slot.at("value").is_null());
        const auto receipt = parse_receipt(slot.at("value"));
        const auto& final = tree.value.at("finalization");
        require(final.is_object() && final.contains("id") && final.contains("timestamp"));
        auto final_id = journal::RunId::parse(string(final.at("id")));
        auto time = journal::Timestamp::parse(string(final.at("timestamp")));
        core::detail::ExpectedErrorCleanup id_cleanup{final_id};
        core::detail::ExpectedErrorCleanup time_cleanup{time};
        require(final_id && time);
        FinalizationFacts facts{std::move(*final_id), std::move(*time)};
        auto expected = project(tx, receipt, facts, b);
        auto canonical = finish_digest(expected, b);
        require(canonical == bytes, Error::association);
        b.usage.output_bytes = canonical.size();
        b.usage.records = records.size();
        const auto& scenario = id.at("scenario_id");
        const auto name =
            scenario.at("value").is_null() ? SV{"unresolved"} : SV{string(scenario.at("value"))};
        const auto leaf_size = add(
            add(add(name.size(), tx.records().front().timestamp.size()), run->text().size()), 19U);
        const auto temporary_size = add(add(run->text().size(), facts.id.text().size()), 16U);
        b.usage.retained_bytes =
            add(sizeof(Validated::Data), add(canonical.size(), add(leaf_size, temporary_size)));
        require(b.usage.retained_bytes <= limits.retained_bytes, Error::capacity);
        const auto safe = [](SV v) {
            return !v.empty() && v != "." && v != ".." && v.find_first_of("/\0", 0, 2) == v.npos;
        };
        require(safe(name), Error::schema);
        require(leaf_size <= 255U && temporary_size <= 255U, Error::capacity);
        auto data = std::make_unique<Validated::Data>();
        data->canonical = std::move(canonical);
        data->digest = *digest;
        data->leaf = std::string{name} + "__" + tx.records().front().timestamp + "__" +
                     run->text() + ".epycbench.json";
        data->temporary = "." + run->text() + "." + facts.id.text() + ".epycbench.tmp";
        require(data->leaf.size() == leaf_size && data->temporary.size() == temporary_size,
                Error::state);
        require(data->leaf.size() <= 255U && data->temporary.size() <= 255U, Error::capacity);
        data->usage = b.usage;
        return Validated{std::move(data)};
    } catch (const Failure& e) {
        return std::unexpected{e};
    } catch (ep::Error e) {
        return std::unexpected{Failure{Error::association, e, {}, {}}};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        return std::unexpected{Failure{Error::capacity, {}, {}, {}}};
    } catch (const J::exception&) {
        return std::unexpected{Failure{Error::schema, {}, {}, {}}};
    }
}
} // namespace epycbench::results::final_document

namespace epycbench::results::final_document {
auto PublicationAccess::publish(Validated&& input, SV root, SV leaf, file::Limits bounds,
                                fs::Durability durability, file::detail::Operations operations)
    -> std::expected<Publication, Failure> {
    if (!input.data_ || input.data_->publication_consumed)
        return std::unexpected{Failure{Error::state, {}, {}, {}}};
    if (leaf != input.data_->leaf || input.canonical().size() > bounds.file_bytes ||
        static_cast<unsigned>(durability) > static_cast<unsigned>(fs::Durability::persistent_block))
        return std::unexpected{Failure{Error::limits, {}, {}, {}}};
    auto storage = file::File::storage(bounds, input.data_->temporary.size());
    if (!storage)
        return std::unexpected{Failure{Error::capacity, {}, {}, storage.error()}};
    // File allocates all names/state before the first creation. A pre-creation
    // failure leaves the input unchanged; any created residual transfers with its
    // immutable document and fixed receipt, including failed file admission.
    auto opened =
        file::detail::Access::open(root, input.data_->temporary, bounds, true, operations);
    if (!opened && !opened.error().created)
        return std::unexpected{Failure{Error::io, {}, {}, opened.error()}};
    input.data_->publication_consumed = true;
    Publication output{std::move(input), {}};
    auto& receipt = output.receipt;
    receipt.temporary_created = true;
    receipt.residual = Residual::temporary;
    const auto failed = [&](file::Failure failure) {
        if (!receipt.failure)
            receipt.failure = Failure{Error::io, {}, {}, failure};
    };
    if (!opened) {
        failed(opened.error());
        receipt.cleanup = opened.error().cleanup;
        return output;
    }
    auto& owner = *opened;
    std::size_t written{};
    const auto bytes = output.document.canonical();
    while (written < bytes.size()) {
        auto result = owner.write_some(
            std::as_bytes(std::span{bytes.data() + written, bytes.size() - written}));
        if (!result) {
            failed(result.error());
            break;
        }
        written += *result;
    }
    receipt.complete_written = written == bytes.size();
    if (!receipt.failure && durability == fs::Durability::persistent_block) {
        auto result = owner.sync_file();
        if (!result)
            failed(result.error());
        else
            receipt.file_synced = true;
    }
    if (!receipt.failure) {
        auto result = owner.rename_no_replace(output.document.final_leaf());
        if (!result) {
            failed(result.error());
            if (result.error().primary.reason == file::Reason::io) {
                const auto native = result.error().primary.native_error;
                const bool definite = native == EACCES || native == EPERM || native == ENOENT ||
                                      native == ENOTDIR || native == EINVAL || native == ENOSYS ||
                                      native == EOPNOTSUPP || native == EROFS;
                if (!definite)
                    receipt.residual = Residual::uncertain;
            }
        } else {
            receipt.rename_observed = true;
            receipt.residual = Residual::final;
        }
    }
    if (!receipt.failure && durability == fs::Durability::persistent_block) {
        auto result = owner.sync_directory();
        if (!result)
            failed(result.error());
        else
            receipt.directory_synced = true;
    }
    receipt.cleanup = owner.close();
    if (!receipt.failure && receipt.cleanup.count)
        failed(file::Failure{receipt.cleanup.issues[0], {}, true});
    return output;
}
auto publish(Validated&& input, SV root, SV leaf, file::Limits bounds, fs::Durability durability)
    -> std::expected<Publication, Failure> {
    return PublicationAccess::publish(std::move(input), root, leaf, bounds, durability,
                                      file::detail::production_operations());
}
} // namespace epycbench::results::final_document
