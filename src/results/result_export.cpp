#include "results/result_export.hpp"

#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <vector>

namespace epycbench::results::result_export {
namespace {
using Json = nlohmann::json;
namespace ep = event_profile;
using core::detail::JsonCleanup;
auto add(std::size_t a, std::size_t b) -> std::size_t {
    if (b > std::numeric_limits<std::size_t>::max() - a) throw Error::overflow;
    return a + b;
}
auto mul(std::size_t a, std::size_t b) -> std::size_t {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) throw Error::overflow;
    return a * b;
}
void require(bool okay) { if (!okay) throw Error::limits; }
struct Measure {
    ep::detail::Shape shape{};
    std::size_t keys{}, longest_key{}, longest_string{}, work{};
    const Limits& limits;
    void debit(std::size_t n) { work = add(work, n); require(work <= limits.work); }
    void bytes(std::size_t n) { shape.bytes = add(shape.bytes, n); require(shape.bytes <= limits.output_bytes); }
    void node(std::size_t depth) {
        debit(1); shape.nodes = add(shape.nodes, 1); shape.depth = std::max(shape.depth, depth);
        require(shape.nodes <= limits.nodes && depth <= limits.depth);
    }
    void text(std::string_view value, bool key) {
        debit(add(value.size(), 1));
        for (const char ch : value) if (static_cast<unsigned char>(ch) > 0x7fU) throw Error::projection;
        shape.text = add(shape.text, value.size()); require(shape.text <= limits.text_bytes);
        bytes(add(mul(6, value.size()), 2));
        if (key) {
            keys = add(keys, value.size()); longest_key = std::max(longest_key, value.size());
        } else longest_string = std::max(longest_string, value.size());
    }
};
// The identical traversal first measures, then builds. A null destination never
// allocates JSON or constructs source strings. Every constructed child has a guard
// before it can receive populated state.
struct Node {
    Measure& measure;
    Json* destination;
    std::size_t depth;
    bool array;
    std::size_t children{};
    void prefix(std::string_view key) {
        if (children++ != 0) measure.bytes(1);
        if (!array) {
            measure.shape.members = add(measure.shape.members, 1);
            require(measure.shape.members <= measure.limits.members);
            measure.text(key, true); measure.bytes(1);
        }
    }
    void insert(std::string_view key, Json& child) {
        if (!destination) return;
        if (array) destination->push_back(std::move(child));
        else core::detail::append_object(*destination, key, child);
    }
    template<class Function> void container(std::string_view key, bool is_array, Function body) {
        prefix(key); measure.node(add(depth, 1)); measure.bytes(2);
        Json child; JsonCleanup cleanup{child};
        if (destination) child = is_array ? Json::array() : Json::object();
        Node nested{measure, destination ? &child : nullptr, add(depth, 1), is_array};
        body(nested); insert(key, child);
    }
    template<class Function> void object(std::string_view key, Function body) { container(key, false, body); }
    template<class Function> void list(std::string_view key, Function body) { container(key, true, body); }
    void scalar(std::string_view key, const Json& value) {
        if (value.is_structured() || value.is_discarded() || value.is_binary()) throw Error::projection;
        prefix(key); measure.node(add(depth, 1));
        if (value.is_string()) measure.text(value.get_ref<const std::string&>(), false);
        else measure.bytes(value.is_boolean() ? 5 : value.is_null() ? 4 : 32);
        if (destination) { Json child(value); JsonCleanup cleanup{child}; insert(key, child); }
    }
    void text(std::string_view key, std::string_view value) {
        prefix(key); measure.node(add(depth, 1)); measure.text(value, false);
        if (destination) { Json child(value); JsonCleanup cleanup{child}; insert(key, child); }
    }
    void index(std::string_view key, std::size_t value) {
        prefix(key); measure.node(add(depth, 1)); measure.bytes(32);
        if (destination) { Json child(value); JsonCleanup cleanup{child}; insert(key, child); }
    }
    void count(std::string_view key, std::size_t value) {
        std::array<char, std::numeric_limits<std::size_t>::digits10 + 2U> buffer{};
        const auto r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (r.ec != std::errc{}) throw Error::overflow;
        text(key, {buffer.data(), static_cast<std::size_t>(r.ptr-buffer.data())});
    }
    void null(std::string_view key) { scalar(key, Json{}); }
};
void fields(Node& out, const Json& in, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) out.scalar(key, in.at(key));
}
void absent(Node& out, std::string_view key, std::string_view reason) {
    out.object(key, [&](Node& n) { n.null("value"); n.text("absent_reason", reason); });
}
template<class Function> void present(Node& out, std::string_view key, Function fn) {
    out.object(key, [&](Node& n) { n.object("value", fn); n.null("absent_reason"); });
}
void scalar_slot(Node& out, std::string_view key, const Json& slot) {
    out.object(key, [&](Node& n) { fields(n, slot, {"value", "absent_reason"}); });
}
void scalar_list(Node& out, std::string_view key, const Json& in) {
    out.list(key, [&](Node& n) { for (const auto& v : in) n.scalar({}, v); });
}
void variant(Node& out, const Json& in) {
    scalar_list(out, "observation_ids", in.at("observation_ids"));
    out.object("descriptive", [&](Node& n) {
        fields(n, in.at("descriptive"), {"count","minimum","maximum","mean","median",
            "population_standard_deviation","coefficient_of_variation","percentile_5","percentile_95"});
    });
    out.object("outliers", [&](Node& n) {
        const auto& v = in.at("outliers");
        fields(n, v, {"median","median_absolute_deviation","flagged_count"});
        scalar_list(n, "flags", v.at("flags"));
    });
}
void calculation(Node& out, const Json& in) {
    for (const auto key : {"first", "second"}) out.object(key, [&](Node& n) { variant(n, in.at(key)); });
    out.object("descriptive_definition", [&](Node& n) {
        fields(n,in.at("descriptive_definition"),{"dispersion","quantile","coefficient_of_variation"});
    });
    scalar_list(out,"block_ids",in.at("block_ids")); scalar_list(out,"ordered_effects",in.at("ordered_effects"));
    out.object("comparison",[&](Node& n) { fields(n,in.at("comparison"),{"mean_log_effect","oriented_geometric_ratio"}); });
    out.object("confidence_interval",[&](Node& n) {
        const auto& ci=in.at("confidence_interval");
        n.object("policy",[&](Node& p) { fields(p,ci.at("policy"),{"replicate_count","confidence","seed"}); });
        for (const auto key : {"log_interval","ratio_interval"}) n.object(key,[&](Node& p) { fields(p,ci.at(key),{"lower","upper"}); });
    });
    out.object("presentation",[&](Node& n) {
        const auto& p=in.at("presentation");
        scalar_slot(n,"percentage_change",p.at("percentage_change"));
        scalar_slot(n,"relative_half_width",p.at("relative_half_width")); n.scalar("excludes_equality",p.at("excludes_equality"));
    });
}
void metric_descriptor(Node& out, detail::Metric metric) {
    using Metric=detail::Metric;
    if (metric==Metric::bandwidth || metric==Metric::latency) {
        present(out,"metric",[&](Node& n) {
            const bool bandwidth=metric==Metric::bandwidth;
            n.text("family",bandwidth?"bandwidth":"dependent_load_latency");
            n.text("unit",bandwidth?"bytes_per_second":"seconds_per_completed_load");
            n.text("better",bandwidth?"higher":"lower");
            n.text("orientation",bandwidth?"candidate_over_baseline":"baseline_over_candidate");
        });
    } else {
        switch(metric) {
        case Metric::no_measurement_family: absent(out,"metric","no_measurement_family"); break;
        case Metric::not_obtained: absent(out,"metric","not_obtained"); break;
        case Metric::not_observed: absent(out,"metric","not_observed"); break;
        case Metric::numerical_failure: absent(out,"metric","numerical_failure"); break;
        case Metric::bandwidth: case Metric::latency: throw Error::projection;
        }
    }
}
void statistics(Node& out, const Json& pair, const Json& projected_statistics, const Json& observations, Profile p) {
    if (p==Profile::observation_table) { absent(out,"statistics","profile_omitted"); return; }
    const auto& value=pair.at("value");
    if (value.contains("absent_reason")) { absent(out,"statistics","not_started"); return; }
    const auto& projection=projected_statistics;
    const bool derived=projection.at("scope")=="derived_analysis";
    const auto selected=detail::select_metric(projection,observations,
        pair.at("pair_id").get_ref<const std::string&>(),out.measure.work,out.measure.limits.work);
    if (!selected) throw selected.error();
    present(out,"statistics",[&](Node& n) {
        n.scalar("scope",projection.at("scope"));
        metric_descriptor(n,*selected);
        const auto& result=projection.at("result");
        const auto& body=result.at("value");
        if (derived && !body.is_null()) n.scalar("validity",body.at("validity"));
        else n.text("validity","not_evaluated");
        if (derived) n.scalar("conclusion",value.at("conclusion")); else n.text("conclusion","not_evaluated");
        if (body.is_null()) {
            absent(n,"calculated",result.at("absent_reason").get_ref<const std::string&>());
        } else if (derived) {
            const auto& a=body.at("analysis");
            if (a.at("value").is_null()) absent(n,"calculated",a.at("absent_reason").get_ref<const std::string&>());
            else present(n,"calculated",[&](Node& c) { calculation(c,a.at("value")); });
        } else present(n,"calculated",[&](Node& c) { calculation(c,body.at("diagnostic")); });
    });
}
void measurement(Node& out, const Json& in) {
    const bool bandwidth=in.at("kind")=="bandwidth";
    out.text("family",bandwidth?"bandwidth":"dependent_load_latency");
    out.scalar("elapsed_nanoseconds",in.at("enclosing_interval").at("elapsed_nanoseconds"));
    out.scalar("aggregate_work",in.at("aggregate_work"));
    out.list("workers",[&](Node& n) {
        std::size_t ordinal{};
        for (const auto& w : in.at("workers")) n.object({},[&](Node& c) {
            c.index("source_worker",ordinal++); c.scalar("worker_id",w.at("worker_id"));
            c.scalar("elapsed_nanoseconds",w.at("interval").at("elapsed_nanoseconds"));
            c.scalar("work",bandwidth?w.at("work").at("logical_bytes_read"):w.at("completed_work"));
            c.object("verification",[&](Node& v) { fields(v,w.at("verification"),{"expected","actual","matched"}); });
        });
    });
}
void observation(Node& out, const Json& in, std::size_t index) {
    out.index("source_observation",index);
    fields(out,in,{"pair_id","block_id","observation_id","included","reason"});
    const auto& component=in.at("component");
    const auto& slot=component.at("slot");
    const auto& index_text=slot.at("slot_index").get_ref<const std::string&>();
    if (index_text.size()!=1 || index_text.front()<'0' || index_text.front()>'3') throw Error::projection;
    out.index("slot_index",static_cast<std::size_t>(index_text.front()-'0'));
    out.scalar("variant_slot",slot.at("variant_slot"));
    const bool factual=component.contains("measurement");
    out.text("kind",factual?"factual":"failed");
    if (factual) out.object("validity",[&](Node& n) { n.scalar("value",component.at("validity").at("disposition")); n.null("absent_reason"); });
    else absent(out,"validity","no_factual_observation");
    scalar_slot(out,"outlier",in.at("outlier"));
    const auto& metric=in.at("metric");
    if (metric.at("value").is_null()) absent(out,"metric",metric.at("absent_reason").get_ref<const std::string&>());
    else present(out,"metric",[&](Node& n) { fields(n,metric.at("value"),{"base","display","unit","better","orientation","formula"}); });
    if (factual) present(out,"measurement",[&](Node& n) { measurement(n,component.at("measurement")); });
    else absent(out,"measurement","no_factual_observation");
    for (const auto key : {"placements","migrations"}) out.list(key,[&](Node& n) {
        if (factual) for (const auto& w : component.at(key)) n.object({},[&](Node& c) { fields(c,w,{"worker_id","state"}); });
    });
}
void experiment(Node& out, const Json& in, std::size_t index, Profile p) {
    out.index("source_experiment",index); out.scalar("type",in.at("type"));
    out.object("status",[&](Node& n) { fields(n,in.at("status"),{"execution","validity","conclusion"}); });
    out.text("row_policy",p==Profile::summary?"analysis_only":"all_retained");
    out.object("retained_counts",[&](Node& n) { n.count("blocks",in.at("blocks").size()); n.count("observations",in.at("observations").size()); });
    out.list("pairs",[&](Node& n) {
        std::size_t ordinal{};
        for (const auto& pair : in.at("comparison")) n.object({},[&](Node& c) {
            const auto pair_index=ordinal++;
            c.index("source_pair",pair_index); c.scalar("pair_id",pair.at("pair_id"));
            const auto& stats=in.at("statistics").at(pair_index);
            if (stats.at("pair_id")!=pair.at("pair_id")) throw Error::projection;
            if (pair.at("value").contains("absent_reason")) absent(c,"roles","not_started");
            else present(c,"roles",[&](Node& r) { fields(r,pair.at("value"),{"baseline_index","candidate_index"}); });
            statistics(c,pair,stats.at("value"),in.at("observations"),p);
        });
    });
    out.list("blocks",[&](Node& n) {
        if (p==Profile::summary) return;
        std::size_t ordinal{};
        for (const auto& b : in.at("blocks")) n.object({},[&](Node& c) {
            c.index("source_block",ordinal++); fields(c,b,{"pair_id","block_id","included","reason"});
            scalar_slot(c,"completion",b.at("completion")); c.count("retained_observations",b.at("observations").size());
        });
    });
    out.list("observations",[&](Node& n) {
        if (p==Profile::summary) return;
        std::size_t ordinal{};
        for (const auto& o : in.at("observations")) n.object({},[&](Node& c) { observation(c,o,ordinal++); });
    });
}
void project(Node& out, const Json& in, Profile p) {
    out.text("schema","epycbench.result-export/v1"); out.text("profile",name(p));
    const auto& extension=in.at("extensions").at("epycbench.private_complete");
    out.object("source",[&](Node& n) {
        n.scalar("schema_revision",in.at("schema").at("revision")); n.scalar("journal_profile",in.at("journal").at("profile"));
        n.scalar("complete_profile",extension.at("profile")); n.scalar("content_digest",in.at("integrity").at("content_digest"));
        n.text("digest_algorithm","sha256"); n.text("digest_profile","source-content-null-digest");
    });
    out.object("status",[&](Node& n) { fields(n,in.at("status"),{"execution","validity","conclusion","observed_execution","recovery_disposition"}); });
    out.object("provenance",[&](Node& n) { n.scalar("origin",extension.at("origin")); n.text("qualification","not_asserted"); });
    out.object("privacy",[&](Node& n) {
        n.text("policy","allowlist-v1"); n.list("omissions",[&](Node& list) {
            for (const auto text : {"authored-and-arbitrary-text","host-and-process-identifiers","paths-and-timestamps","error-and-human-evidence","opaque-artifacts","complete-monitoring-and-topology"}) list.text({},text);
        });
    });
    out.list("experiments",[&](Node& n) {
        std::size_t ordinal{};
        for (const auto& e : in.at("experiments")) n.object({},[&](Node& c) { experiment(c,e,ordinal++,p); });
    });
}
} // namespace

auto detail::select_metric(const Json& projection, const Json& observations,
    std::string_view pair_id,std::size_t& work,std::size_t limit) noexcept
    -> std::expected<Metric,Error> {
    try {
        const auto spend=[&](std::size_t amount) { work=add(work,amount); require(work<=limit); };
        const auto equal=core::detail::json_string_equal<Json>;
        const auto coupled=[&](const Json& value,bool bandwidth) {
            if (!equal(value.at("unit"),bandwidth?"bytes_per_second":"seconds_per_completed_load") ||
                !equal(value.at("better"),bandwidth?"higher":"lower") ||
                !equal(value.at("orientation"),bandwidth?"candidate_over_baseline":"baseline_over_candidate")) throw Error::projection;
            return bandwidth?Metric::bandwidth:Metric::latency;
        };
        spend(1);
        const bool derived=equal(projection.at("scope"),"derived_analysis");
        if (!derived && !equal(projection.at("scope"),"diagnostic_partial")) throw Error::projection;
        const auto& body=projection.at("result").at("value");
        if (body.is_null()) return Metric::not_obtained;
        if (derived) {
            const auto& slot=body.at("metric");
            if (slot.at("value").is_null()) {
                if (!equal(slot.at("absent_reason"),"no_measurement_family")) throw Error::projection;
                return Metric::no_measurement_family;
            }
            const auto& value=slot.at("value");
            if (equal(value.at("family"),"bandwidth")) return coupled(value,true);
            if (equal(value.at("family"),"dependent_load_latency")) return coupled(value,false);
            throw Error::projection;
        }
        bool numerical_failure{};
        for (const auto& row : observations) {
            const auto& id=row.at("pair_id").get_ref<const std::string&>();
            spend(add(1,id.size()));
            if (id!=pair_id) continue;
            const auto& component=row.at("component");
            if (!component.contains("measurement")) continue;
            const auto& slot=row.at("metric");
            if (slot.at("value").is_null()) {
                numerical_failure=numerical_failure || equal(slot.at("absent_reason"),"numerical_failure");
                continue;
            }
            const auto& kind=component.at("measurement").at("kind");
            if (equal(kind,"bandwidth")) return coupled(slot.at("value"),true);
            if (equal(kind,"latency")) return coupled(slot.at("value"),false);
            throw Error::projection;
        }
        return numerical_failure?Metric::numerical_failure:Metric::not_observed;
    } catch (Error e) { return std::unexpected{e}; }
      catch (const std::bad_alloc&) { return std::unexpected{Error::allocation}; }
      catch (...) { return std::unexpected{Error::projection}; }
}

auto detail::renderer_stock(RendererShape m) noexcept -> std::expected<std::size_t,Error> {
    try {
        const auto slot=mul(2,add(mul(2,sizeof(void*)),add(sizeof(std::vector<std::uint32_t>),sizeof(std::vector<std::uint16_t>))));
        const auto path=add(1,mul(m.depth,std::max(add(m.longest_key,4),std::size_t{24})));
        return add(add(mul(m.members,slot),mul(6,m.key_bytes)),
            add(mul(4,m.longest_string),mul(8,mul(add(m.depth,2),add(path,1)))));
    } catch (Error e) { return std::unexpected{e}; }
}

auto profile(std::string_view text) noexcept -> std::optional<Profile> {
    for (const auto p : {Profile::complete,Profile::shareable,Profile::summary,Profile::observation_table})
        if (name(p)==text) return p;
    return {};
}
auto name(Profile p) noexcept -> std::string_view {
    switch(p) {
    case Profile::complete:return "complete";
    case Profile::shareable:return "shareable";
    case Profile::summary:return "summary";
    case Profile::observation_table:return "observation-table";
    }
    return {};
}
auto render(const final_document::Validated& document, Profile p, Limits limits, Usage* usage)
    -> std::expected<std::string,Error> {
    if (usage) *usage={};
    try {
        if (!document.digest() || document.canonical().empty() || p==Profile::complete || name(p).empty()) return std::unexpected{Error::state};
        require(document.canonical().size()<=limits.input_bytes && limits.depth<=64);
        ep::Limits parse;
        parse.body_bytes=limits.input_bytes; parse.nodes=limits.nodes; parse.depth=limits.depth;
        parse.text_bytes=limits.text_bytes; parse.canonical_bytes=limits.input_bytes;
        parse.candidate_bytes=limits.workspace_bytes; parse.work=limits.work;
        ep::detail::Work work{0,limits.work,0,limits.workspace_bytes};
        Usage measured; measured.source=ep::detail::shape(document.canonical(),parse,work);
        require(measured.source.members<=limits.members);
        measured.source_tree=ep::detail::json_stock(measured.source);
        // Validated already owns the canonical, semantically checked source.
        // Projection needs only an ordinary DOM, not C2 canonicalization,
        // UTF ordering or digest-preimage scratch. Admit the DOM, input/lexer
        // buffers and fixed parser-control stock before constructing that DOM.
        measured.workspace=ep::detail::dom_workspace(measured.source,limits.depth);
        require(measured.workspace<=limits.workspace_bytes && measured.source_tree<=limits.tree_bytes);
        work.reserve_peak(measured.workspace);
        measured.simultaneous=add(document.canonical().size(),add(measured.workspace,measured.source_tree));
        auto source=ep::detail::parse_tree_dom(document.canonical(),parse,work);
        JsonCleanup source_cleanup{source};
        Measure m{{},0,0,0,work.used,limits}; m.node(1); m.bytes(2);
        Node measuring{m,nullptr,1,false}; project(measuring,source,p);
        measured.output=m.shape; measured.output_tree=ep::detail::json_stock(m.shape);
        const auto scratch=detail::renderer_stock({m.shape.members,m.keys,m.longest_key,m.longest_string,m.shape.depth});
        if (!scratch) return std::unexpected{scratch.error()};
        measured.renderer=*scratch;
        require(measured.output_tree<=limits.tree_bytes && measured.renderer<=limits.renderer_bytes);
        measured.simultaneous=std::max(measured.simultaneous,add(document.canonical().size(),add(measured.source_tree,measured.output_tree)));
        measured.simultaneous=std::max(measured.simultaneous,add(document.canonical().size(),add(measured.output_tree,add(measured.renderer,add(m.shape.bytes,1)))));
        Json output=Json::object(); JsonCleanup output_cleanup{output};
        Measure building{{},0,0,0,m.work,limits}; building.node(1); building.bytes(2);
        Node builder{building,&output,1,false}; project(builder,source,p);
        core::detail::retire_json(source);
        measured.work=building.work;
        auto encoded=core::canonicalize_json_jcs(output,{m.shape.bytes,limits.depth,m.shape.nodes});
        core::detail::ExpectedErrorCleanup encoded_cleanup{encoded};
        if (!encoded) return std::unexpected{Error::canonical};
        require(encoded->size()<=limits.output_bytes);
        if (usage) *usage=measured;
        return std::move(*encoded);
    } catch (Error e) { return std::unexpected{e}; }
      catch (ep::Error) { return std::unexpected{Error::limits}; }
      catch (const std::bad_alloc&) { return std::unexpected{Error::allocation}; }
      catch (...) { return std::unexpected{Error::projection}; }
}
} // namespace epycbench::results::result_export
