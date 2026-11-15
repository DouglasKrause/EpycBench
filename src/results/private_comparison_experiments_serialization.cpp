#include "results/private_comparison_experiments_serialization.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace epycbench::results::comparison_experiments_serialization {
using Experiment = scenario::ResolvedComparisonExperiment;
using Variant = scenario::ResolvedComparisonVariant;
using Worker = scenario::ResolvedProcessingUnit;
using Common = scenario::ResolvedMemoryPlacement;
using PerWorker = scenario::ResolvedPerWorkerMemoryPlacement;
using Node = scenario::ResolvedMemoryNode;
using Mapping = scenario::ResolvedPerWorkerMemoryNode;
using NodeSet = std::set<core::LinuxNumaNodeId>;
namespace detail {
struct State {
    std::vector<Experiment> experiments;
    std::string canonical;
    std::optional<core::Sha256Digest> digest;
    Origin origin{Origin::decoded_unassociated};
    Usage usage;
};
auto checked_add(std::size_t& a, std::size_t b) noexcept -> bool {
    if (b > std::numeric_limits<std::size_t>::max() - a) return false;
    a += b;
    return true;
}
auto checked_multiply(std::size_t a, std::size_t b) noexcept -> std::optional<std::size_t> {
    if (b && a > std::numeric_limits<std::size_t>::max() / b) return std::nullopt;
    return a * b;
}
} // namespace detail
namespace {
using detail::State;
constexpr std::string_view hex_digits = "0123456789abcdef";
constexpr std::array quantities{
    &Usage::experiments,&Usage::workers,&Usage::variants,&Usage::nodes,&Usage::set_members,
    &Usage::mappings,&Usage::candidates,&Usage::cpu_members,&Usage::cpu_ranges,&Usage::maximum_cpu_members,
    &Usage::cpu_input_bytes,&Usage::text_bytes,&Usage::maximum_text,&Usage::range_components,
    &Usage::canonical_bytes,&Usage::staged_bytes,&Usage::retained_bytes,&Usage::consumer_simultaneous_bytes};
void need(bool condition, Error error = Error::schema) { if (!condition) throw error; }
void add(std::size_t& a, std::size_t b) { need(detail::checked_add(a,b),Error::capacity); }
auto mul(std::size_t a, std::size_t b) -> std::size_t {
    const auto result=detail::checked_multiply(a,b);
    need(result.has_value(),Error::capacity);
    return *result;
}
struct Decimal {
    std::array<char,20> data{};
    std::size_t length{};
    explicit Decimal(std::uint64_t value) {
        const auto result=std::to_chars(data.data(),data.data()+data.size(),value);
        length=static_cast<std::size_t>(result.ptr-data.data());
    }
    auto view() const noexcept -> std::string_view { return {data.data(),length}; }
};
auto nibble(char value) -> unsigned {
    if (value>='0' && value<='9') return static_cast<unsigned>(value-'0');
    need(value>='a' && value<='f',Error::encoding);
    return static_cast<unsigned>(value-'a')+10U;
}
struct Text {
    std::string_view bytes;
    bool encoded{};
    auto size() const noexcept -> std::size_t { return encoded ? bytes.size()/2:bytes.size(); }
    auto at(std::size_t index) const -> char {
        if (!encoded) return bytes[index];
        return static_cast<char>((nibble(bytes[2*index])<<4U)|nibble(bytes[2*index+1]));
    }
    auto owning() const -> std::string {
        std::string result; result.reserve(size());
        for (std::size_t i=0;i<size();++i) result.push_back(at(i));
        return result;
    }
};
struct Counts {
    Usage q;
    std::size_t dynamic{}, stage{};
    const Limits* limits{};
    void check() const {
        if (limits) for (const auto field:quantities)
            need(q.*field<=(*limits).*field,Error::capacity);
    }
    void row(std::size_t& count,std::size_t bytes,bool staged) {
        add(count,1); add(dynamic,bytes); if (staged) add(stage,bytes); check();
    }
    void text(Text value,bool core_id=false) {
        add(q.text_bytes,value.size()); add(dynamic,value.size()); add(stage,value.size());
        q.maximum_text=std::max(q.maximum_text,value.size()); check();
        need(!limits || stage<=limits->staged_bytes,Error::capacity);
        if (core_id) {
            std::size_t components=1;
            for (std::size_t i=0;i<value.size();++i) if (value.at(i)==',') { add(components,1); need(!limits || components<=limits->range_components,Error::capacity); }
            q.range_components=std::max(q.range_components,components);
        }
        check();
    }
    void finish() { q.staged_bytes=std::max(q.staged_bytes,stage); stage=0; check(); }
};
struct Sink {
    std::string* output{};
    std::size_t size{};
    std::size_t ceiling{SIZE_MAX};
    void raw(std::string_view value) { add(size,value.size()); need(size<=ceiling,Error::capacity); if (output) output->append(value); }
    void character(char value) { add(size,1); need(size<=ceiling,Error::capacity); if (output) output->push_back(value); }
    void number(std::uint64_t value) {
        const Decimal decimal{value}; character('"'); raw(decimal.view()); character('"');
    }
    void boolean(bool value) { raw(value?"true":"false"); }
    void text(std::string_view value,Counts& count,bool core_id=false) {
        need(size<=ceiling && ceiling-size>=2 && value.size()<=(ceiling-size-2)/2,Error::capacity);
        count.text({value,false},core_id); character('"');
        for (const char octet:value) {
            const auto byte=static_cast<unsigned char>(octet);
            character(hex_digits[byte>>4U]); character(hex_digits[byte&15U]);
        }
        character('"');
    }
};
void emit_worker(Sink& out,const Worker& value,Counts& count) {
    out.character('['); out.text(value.stable_id.value(),count); out.character(',');
    out.number(value.linux_cpu.value()); out.character(','); out.text(value.core_id.value(),count,true); out.character(']');
}
void emit_node(Sink& out,const Node& value,Counts& count) {
    out.character('['); out.text(value.stable_id.value(),count); out.character(',');
    out.number(value.linux_node.value()); out.character(']');
}
void emit_ratio(Sink& out,const scenario::ExactRatio& value) {
    out.character('['); out.number(value.numerator()); out.character(','); out.number(value.denominator()); out.character(']');
}
template<class P> void emit_scalars(Sink& out,const P& value,Counts& count) {
    need(value.policy==scenario::MemoryPolicy::bind && value.page_mode==scenario::PageMode::base,Error::reference);
    out.raw(",\"base\","); out.number(value.page_size.value()); out.character(','); emit_ratio(out,value.placement_threshold);
    out.character(','); out.boolean(value.prefault); out.character(','); out.boolean(value.verify_before);
    out.character(','); out.boolean(value.verify_after); out.character(','); out.text(value.source_pointer,count);
}
void emit_placement(Sink& out,const scenario::ComparisonMemoryPlacement& value,Counts& count) {
    bool first=true;
    if (const auto* common=std::get_if<Common>(&value)) {
        out.raw("[\"common\",\"bind\",[");
        for (const auto& node:common->nodes) {
            count.row(count.q.nodes,sizeof(Node),true);
            if (!first) out.character(',');
            first=false; emit_node(out,node,count);
        }
        out.raw("],["); first=true;
        for (const auto member:common->linux_nodes) {
            count.row(count.q.set_members,sizeof(NodeSet::value_type),true);
            if (!first) out.character(',');
            first=false; out.number(member.value());
        }
        out.character(']'); emit_scalars(out,*common,count);
    } else {
        const auto& per=std::get<PerWorker>(value); out.raw("[\"per_worker\",\"bind\",[");
        for (const auto& mapping:per.worker_nodes) {
            count.row(count.q.mappings,sizeof(Mapping),true);
            if (!first) out.character(',');
            first=false; out.character('['); emit_worker(out,mapping.worker,count); out.character(',');
            emit_node(out,mapping.node,count); out.character(']');
        }
        out.character(']'); emit_scalars(out,per,count);
    }
    out.character(']');
}
auto order_token(scenario::MatrixComparisonOrder order) -> std::string_view {
    switch (order) {
    case scenario::MatrixComparisonOrder::randomized_abba:return "randomized-abba";
    case scenario::MatrixComparisonOrder::randomized_baab:return "randomized-baab";
    case scenario::MatrixComparisonOrder::randomized_balanced:return "randomized-balanced";
    }
    throw Error::reference;
}
void cpu_count(Counts& count,std::uint32_t value,std::optional<std::uint32_t>& previous,
               std::size_t& members,std::size_t& input) {
    need(!previous || *previous<value,Error::reference);
    add(count.q.cpu_members,1); add(members,1);
    if (!previous || static_cast<std::uint64_t>(*previous)+1!=value)
        count.row(count.q.cpu_ranges,2*sizeof(std::uint32_t),true);
    const Decimal decimal{value}; add(input,decimal.length); if (previous) add(input,1);
    previous=value; count.check();
}
void cpu_finish(Counts& count,std::size_t members,std::size_t input) {
    count.q.maximum_cpu_members=std::max(count.q.maximum_cpu_members,members);
    count.q.cpu_input_bytes=std::max(count.q.cpu_input_bytes,input); count.check();
}
void emit(Sink& out,const std::vector<Experiment>& records,Counts& count) {
    need(!count.limits || records.size()<=count.limits->experiments,Error::capacity);
    out.raw("{\"comparison_experiments\":["); bool first_record=true;
    for (const auto& record:records) {
        count.row(count.q.experiments,sizeof(Experiment),false);
        if (!first_record) out.character(',');
        first_record=false; out.character('['); out.number(record.experiment_index); out.character(',');
        out.text(record.experiment_id,count); out.character(','); out.text(record.workload_id,count); out.raw(",[");
        bool first=true;
        for (const auto& worker:record.workers) {
            count.row(count.q.workers,sizeof(Worker),true);
            if (!first) out.character(',');
            first=false; emit_worker(out,worker,count);
        }
        out.raw("],["); first=true;
        // Cardinality is checked before any expanded CpuSet traversal.
        need(record.linux_cpus.cardinality()<=SIZE_MAX-count.q.cpu_members,Error::capacity);
        if (count.limits) {
            need(record.linux_cpus.cardinality()<=count.limits->cpu_members-count.q.cpu_members,Error::capacity);
            need(record.linux_cpus.cardinality()<=count.limits->maximum_cpu_members,Error::capacity);
        }
        std::optional<std::uint32_t> previous; std::size_t members{},input{};
        for (const auto cpu:record.linux_cpus) {
            cpu_count(count,cpu.value(),previous,members,input);
            if (!first) out.character(',');
            first=false; out.number(cpu.value());
        }
        cpu_finish(count,members,input); out.raw("],["); first=true;
        for (const auto& variant:record.variants) {
            count.row(count.q.variants,sizeof(Variant),true);
            if (!first) out.character(',');
            first=false; out.character('['); out.number(variant.variant_index); out.character(',');
            out.text(variant.variant_id,count); out.character(','); emit_placement(out,variant.memory_placement,count); out.character(']');
        }
        out.raw("],"); out.text(record.baseline_variant_id,count); out.raw(",["); first=true;
        for (const auto& candidate:record.candidate_variant_ids) {
            count.row(count.q.candidates,sizeof(std::string),true);
            if (!first) out.character(',');
            first=false; out.text(candidate,count);
        }
        out.raw("],["); emit_ratio(out,record.comparison.practical_effect); out.character(',');
        emit_ratio(out,record.comparison.confidence); out.raw(",\""); out.raw(order_token(record.comparison.order)); out.raw("\",");
        if (record.comparison.order_seed) out.number(*record.comparison.order_seed); else out.raw("null");
        out.raw("],[");
        if (record.practical_effect_origin.kind==scenario::ValueOriginKind::schema_default) out.raw("\"schema_default\",");
        else { need(record.practical_effect_origin.kind==scenario::ValueOriginKind::experiment_override,Error::reference); out.raw("\"experiment_override\","); }
        if (record.practical_effect_origin.source_pointer) out.text(*record.practical_effect_origin.source_pointer,count); else out.raw("null");
        out.raw("]]"); count.finish();
    }
    out.raw("],\"expansion_profile\":\"epycbench.node-pair-matrix.v1\",\"schema\":\""); out.raw(profile); out.raw("\"}");
    count.q.canonical_bytes=out.size; count.check();
}
template<class Id> auto factory(Text text) -> Id {
    const auto input=text.owning(); auto value=Id::parse(input);
    core::detail::ExpectedErrorCleanup cleanup{value}; need(value.has_value(),Error::typed_factory_failure);
    return std::move(*value);
}
struct Ratio { std::uint64_t numerator{},denominator{}; };
auto typed_ratio(Ratio value) -> scenario::ExactRatio {
    auto result=scenario::ExactRatio::create(value.numerator,value.denominator);
    core::detail::ExpectedErrorCleanup cleanup{result}; need(result.has_value(),Error::typed_factory_failure); return *result;
}
struct Scalars { std::uint64_t page{}; Ratio ratio; bool prefault{},before{},after{}; Text pointer; };
struct Parser {
    std::string_view body; std::size_t pos{}; Counts count; State* output{};
    void token(std::string_view value) { need(body.substr(pos).starts_with(value)); pos+=value.size(); }
    auto take(std::string_view value) -> bool {
        if (!body.substr(pos).starts_with(value)) return false;
        pos+=value.size(); return true;
    }
    auto number(std::uint64_t maximum=UINT64_MAX) -> std::uint64_t {
        token("\""); const auto begin=pos;
        while (pos<body.size() && body[pos]>='0' && body[pos]<='9') ++pos;
        need(pos>begin && (pos-begin==1 || body[begin]!='0'),Error::encoding);
        std::uint64_t value{}; const auto result=std::from_chars(body.data()+begin,body.data()+pos,value);
        need(result.ec==std::errc{} && value<=maximum,Error::encoding); token("\""); return value;
    }
    auto text(bool core_id=false) -> Text {
        token("\""); const auto begin=pos;
        while (pos<body.size() && body[pos]!='"') { (void)nibble(body[pos]); ++pos; }
        need((pos-begin)%2==0,Error::encoding);
        const Text value{body.substr(begin,pos-begin),true}; token("\""); count.text(value,core_id); return value;
    }
    auto boolean() -> bool { if (take("true")) return true; token("false"); return false; }
    auto next(bool& first) -> bool {
        if (take("]")) return false;
        if (!first) token(",");
        first=false; return true;
    }
    auto ratio() -> Ratio {
        token("["); Ratio value; value.numerator=number(); token(","); value.denominator=number(); token("]");
        need(value.denominator && value.numerator<=value.denominator && std::gcd(value.numerator,value.denominator)==1,Error::reference);
        return value;
    }
    auto worker() -> std::optional<Worker> {
        token("["); const auto pu=text(); token(","); const auto cpu=number(UINT32_MAX); token(","); const auto core=text(true); token("]");
        if (!output) return std::nullopt;
        return Worker{factory<topology::StableProcessingUnitId>(pu),core::LinuxCpuId{static_cast<std::uint32_t>(cpu)},factory<topology::StableCoreId>(core)};
    }
    auto node() -> std::optional<Node> {
        token("["); const auto id=text(); token(","); const auto linux=number(UINT32_MAX); token("]");
        if (!output) return std::nullopt;
        return Node{factory<topology::StableNumaNodeId>(id),core::LinuxNumaNodeId{static_cast<std::uint32_t>(linux)}};
    }
    auto mapping() -> std::optional<Mapping> {
        token("["); auto owner=worker(); token(","); auto target=node(); token("]");
        if (!output) return std::nullopt;
        return Mapping{std::move(*owner),std::move(*target)};
    }
    auto scalars() -> Scalars {
        token(",\"base\","); Scalars value; value.page=number(); token(","); value.ratio=ratio(); token(",");
        value.prefault=boolean(); token(","); value.before=boolean(); token(","); value.after=boolean(); token(","); value.pointer=text(); return value;
    }
    auto placement() -> std::optional<scenario::ComparisonMemoryPlacement> {
        token("["); const bool common=take("\"common\""); if (!common) token("\"per_worker\""); token(",\"bind\",[");
        bool first=true; std::vector<Node> nodes; NodeSet members; std::vector<Mapping> mappings;
        while (next(first)) {
            if (common) { count.row(count.q.nodes,sizeof(Node),true); auto value=node(); if (output) nodes.push_back(std::move(*value)); }
            else { count.row(count.q.mappings,sizeof(Mapping),true); auto value=mapping(); if (output) mappings.push_back(std::move(*value)); }
        }
        if (common) {
            token(",["); first=true; std::optional<std::uint32_t> previous;
            while (next(first)) {
                count.row(count.q.set_members,sizeof(NodeSet::value_type),true);
                const auto value=static_cast<std::uint32_t>(number(UINT32_MAX)); need(!previous || *previous<value,Error::reference); previous=value;
                if (output) members.emplace(value);
            }
        }
        const auto value=scalars(); token("]");
        if (!output) return std::nullopt;
        if (common) return scenario::ComparisonMemoryPlacement{Common{scenario::MemoryPolicy::bind,std::move(nodes),std::move(members),
            scenario::PageMode::base,core::ByteCount::from_base_units(value.page),typed_ratio(value.ratio),
            value.prefault,value.before,value.after,value.pointer.owning()}};
        return scenario::ComparisonMemoryPlacement{PerWorker{scenario::MemoryPolicy::bind,std::move(mappings),scenario::PageMode::base,
            core::ByteCount::from_base_units(value.page),typed_ratio(value.ratio),value.prefault,value.before,value.after,value.pointer.owning()}};
    }
    void experiment() {
        count.row(count.q.experiments,sizeof(Experiment),false); token("["); const auto index=static_cast<std::size_t>(number(SIZE_MAX)); token(",");
        const auto id=text(); token(","); const auto workload=text(); token(",[");
        std::vector<Worker> workers; bool first=true;
        while (next(first)) { count.row(count.q.workers,sizeof(Worker),true); auto value=worker(); if (output) workers.push_back(std::move(*value)); }
        token(",["); first=true; std::string cpu_text; std::optional<std::uint32_t> previous; std::size_t members{},input{};
        while (next(first)) {
            const auto value=static_cast<std::uint32_t>(number(UINT32_MAX));
            cpu_count(count,value,previous,members,input);
            if (output) { if (!cpu_text.empty()) cpu_text.push_back(','); const Decimal decimal{value}; cpu_text.append(decimal.view()); }
        }
        cpu_finish(count,members,input); need(members>0,Error::reference);
        core::CpuSet cpus;
        if (output) {
            auto parsed=core::CpuSet::parse(cpu_text); need(parsed.has_value(),Error::typed_factory_failure); cpus=std::move(*parsed);
        }
        // Input and factory range vectors retire before any later placement factory is called.
        std::string{}.swap(cpu_text);
        token(",["); first=true; std::vector<Variant> variants;
        while (next(first)) {
            count.row(count.q.variants,sizeof(Variant),true); token("["); const auto vi=static_cast<std::size_t>(number(SIZE_MAX)); token(",");
            const auto name=text(); token(","); auto value=placement(); token("]");
            if (output) variants.emplace_back(vi,name.owning(),std::move(*value));
        }
        token(","); const auto baseline=text(); token(",["); first=true; std::vector<std::string> candidates;
        while (next(first)) { count.row(count.q.candidates,sizeof(std::string),true); const auto value=text(); if (output) candidates.push_back(value.owning()); }
        token(",["); const auto practical=ratio(); token(","); const auto confidence=ratio(); token(",");
        scenario::MatrixComparisonOrder order;
        if (take("\"randomized-abba\"")) order=scenario::MatrixComparisonOrder::randomized_abba;
        else if (take("\"randomized-baab\"")) order=scenario::MatrixComparisonOrder::randomized_baab;
        else { token("\"randomized-balanced\""); order=scenario::MatrixComparisonOrder::randomized_balanced; }
        token(","); std::optional<std::uint64_t> seed; if (!take("null")) seed=number(); token("],[");
        const bool inherited=take("\"schema_default\""); if (!inherited) token("\"experiment_override\""); token(",");
        std::optional<Text> pointer; if (!take("null")) pointer=text(); token("]]"); count.finish();
        if (output) {
            std::optional<std::string> owned_pointer; if (pointer) owned_pointer=pointer->owning();
            output->experiments.push_back(Experiment{index,id.owning(),workload.owning(),std::move(workers),std::move(cpus),std::move(variants),
                baseline.owning(),std::move(candidates),{typed_ratio(practical),typed_ratio(confidence),order,seed},
                {inherited?scenario::ValueOriginKind::schema_default:scenario::ValueOriginKind::experiment_override,std::move(owned_pointer)}});
        }
    }
    void run() {
        token("{\"comparison_experiments\":["); bool first=true; while (next(first)) experiment();
        token(",\"expansion_profile\":\"epycbench.node-pair-matrix.v1\",\"schema\":\""); token(profile); token("\"}");
        need(pos==body.size(),Error::encoding); count.q.canonical_bytes=body.size(); count.check();
    }
};
auto suffix_id(std::string_view text,std::string_view prefix,std::uint32_t id) -> bool {
    const Decimal number{id}; return text.starts_with(prefix) && text.substr(prefix.size())==number.view();
}
auto pointer_valid(std::string_view value,std::size_t experiment,std::optional<std::size_t> variant) -> bool {
    const Decimal index{experiment};
    if (!value.starts_with("/experiments/")) return false;
    value.remove_prefix(13);
    if (!value.starts_with(index.view())) return false;
    value.remove_prefix(index.length);
    if (!variant) return value=="/memory";
    const Decimal subindex{*variant};
    if (!value.starts_with("/variants/")) return false;
    value.remove_prefix(10);
    if (!value.starts_with(subindex.view())) return false;
    value.remove_prefix(subindex.length); return value=="/memory";
}
auto core_contains(std::string_view id,std::uint32_t cpu) -> bool {
    const auto separator=id.rfind("-set"); if (separator==std::string_view::npos) return false;
    id.remove_prefix(separator+4); bool found=false;
    while (!id.empty()) {
        const auto comma=id.find(','); const auto component=id.substr(0,comma);
        const auto dash=component.find('-'); std::uint32_t first{},last{};
        const auto initial=component.substr(0,dash);
        const auto a=std::from_chars(initial.data(),initial.data()+initial.size(),first);
        if (a.ec!=std::errc{} || a.ptr!=initial.data()+initial.size()) return false;
        last=first;
        if (dash!=std::string_view::npos) {
            const auto final=component.substr(dash+1);
            const auto b=std::from_chars(final.data(),final.data()+final.size(),last);
            if (b.ec!=std::errc{} || b.ptr!=final.data()+final.size()) return false;
        }
        found=found || (first<=cpu && cpu<=last);
        if (comma==std::string_view::npos) break;
        id.remove_prefix(comma+1);
    }
    return found;
}
auto identifier(std::string_view value) -> bool {
    if (value.empty() || value.size()>64 || value[0]<'a' || value[0]>'z') return false;
    for (const char c:value) if (!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-')) return false;
    return true;
}
auto origin_pointer(std::string_view value,std::size_t experiment) -> bool {
    if (!value.starts_with("/experiments/")) return false;
    value.remove_prefix(13); const Decimal index{experiment};
    if (!value.starts_with(index.view())) return false;
    value.remove_prefix(index.length); return value=="/compare/practical_effect";
}
struct Identity { const Worker* worker; const Node* node; std::size_t experiment; };
enum class NameKind { experiment,variant,candidate };
struct Name { std::string_view text; std::size_t experiment; NameKind kind; };
struct IdentityLess {
    auto operator()(const Identity& a,const Identity& b) const noexcept -> bool {
        if (a.worker->linux_cpu!=b.worker->linux_cpu) return a.worker->linux_cpu<b.worker->linux_cpu;
        if (a.experiment!=b.experiment) return a.experiment<b.experiment;
        return (a.node!=nullptr)<(b.node!=nullptr);
    }
};
struct NameLess {
    auto operator()(const Name& a,const Name& b) const noexcept -> bool {
        if (a.kind!=b.kind) return a.kind<b.kind;
        if (a.experiment!=b.experiment) return a.experiment<b.experiment;
        return a.text<b.text;
    }
};
void validate_worker(const Worker& value) {
    need(suffix_id(value.stable_id.value(),"pu/",value.linux_cpu.value()) &&
        core_contains(value.core_id.value(),value.linux_cpu.value()),Error::reference);
}
void validate_node(const Node& value) { need(suffix_id(value.stable_id.value(),"numa/",value.linux_node.value()),Error::reference); }
template<class P> void validate_scalars(const P& value,std::optional<std::uint64_t>& page,std::size_t experiment,std::size_t variant) {
    need(value.page_size.value()>0 && (!page || *page==value.page_size.value()),Error::reference);
    page=value.page_size.value(); need(pointer_valid(value.source_pointer,experiment,variant),Error::reference);
}
void validate(const State& state) {
    std::vector<Identity> identities; std::vector<Name> names;
    identities.reserve(state.usage.workers+state.usage.mappings);
    names.reserve(state.usage.experiments+state.usage.variants+state.usage.candidates);
    std::optional<std::size_t> previous; std::optional<std::uint64_t> page;
    for (const auto& record:state.experiments) {
        need(!previous || *previous<record.experiment_index,Error::reference); previous=record.experiment_index;
        need(identifier(record.experiment_id) && identifier(record.workload_id) && identifier(record.baseline_variant_id),Error::reference);
        names.push_back({record.experiment_id,0,NameKind::experiment});
        need(!record.workers.empty() && record.workers.size()==record.linux_cpus.cardinality(),Error::reference);
        for (const auto& worker:record.workers) {
            validate_worker(worker); need(record.linux_cpus.contains(worker.linux_cpu),Error::reference);
            identities.push_back({&worker,nullptr,record.experiment_index});
        }
        need(record.variants.size()>=2 && !record.candidate_variant_ids.empty(),Error::reference);
        std::size_t variant_index{}; const PerWorker* first_mapping=nullptr;
        for (const auto& variant:record.variants) {
            need(variant.variant_index==variant_index && identifier(variant.variant_id),Error::reference); ++variant_index;
            names.push_back({variant.variant_id,record.experiment_index,NameKind::variant});
            if (const auto* common=std::get_if<Common>(&variant.memory_placement)) {
                validate_scalars(*common,page,record.experiment_index,variant.variant_index);
                need(!common->nodes.empty() && common->nodes.size()==common->linux_nodes.size(),Error::reference);
                // Single numeric copy reuses Q after all owning construction staging has retired.
                NodeSet remaining=common->linux_nodes;
                for (const auto& node:common->nodes) { validate_node(node); need(remaining.erase(node.linux_node)==1,Error::reference); }
            } else {
                const auto& per=std::get<PerWorker>(variant.memory_placement);
                validate_scalars(per,page,record.experiment_index,variant.variant_index);
                need(per.worker_nodes.size()==record.workers.size(),Error::reference);
                if (first_mapping) need(per.worker_nodes==first_mapping->worker_nodes,Error::reference); else first_mapping=&per;
                std::size_t i{};
                for (const auto& mapping:per.worker_nodes) {
                    need(mapping.worker==record.workers[i],Error::reference); ++i;
                    validate_node(mapping.node); identities.push_back({&mapping.worker,&mapping.node,record.experiment_index});
                }
            }
        }
        for (const auto& candidate:record.candidate_variant_ids) {
            need(identifier(candidate),Error::reference); names.push_back({candidate,record.experiment_index,NameKind::candidate});
        }
        const auto& origin=record.practical_effect_origin;
        if (origin.kind==scenario::ValueOriginKind::schema_default) {
            need(!origin.source_pointer && record.comparison.practical_effect.numerator()==1 &&
                record.comparison.practical_effect.denominator()==100,Error::reference);
        } else {
            need(origin.kind==scenario::ValueOriginKind::experiment_override && origin.source_pointer &&
                origin_pointer(*origin.source_pointer,record.experiment_index),Error::reference);
        }
    }
    std::ranges::sort(names,NameLess{});
    for (std::size_t i=1;i<names.size();++i) {
        const auto& a=names[i-1]; const auto& b=names[i];
        need(a.kind!=b.kind || a.experiment!=b.experiment || a.text!=b.text,Error::reference);
    }
    std::ranges::sort(identities,IdentityLess{}); const Node* node=nullptr;
    for (std::size_t i=0;i<identities.size();++i) {
        const auto& current=identities[i];
        if (i && identities[i-1].worker->linux_cpu==current.worker->linux_cpu) {
            const auto& previous_identity=identities[i-1];
            need(*previous_identity.worker==*current.worker,Error::reference);
            need(previous_identity.experiment!=current.experiment || previous_identity.node || current.node,Error::reference);
        } else node=nullptr;
        if (current.node) { need(!node || *node==*current.node,Error::reference); node=current.node; }
    }
}
auto admit(Counts count,const Limits& limits,bool decoding) -> Usage {
    std::size_t identities=count.q.workers; add(identities,count.q.mappings);
    std::size_t names=count.q.experiments; add(names,count.q.variants); add(names,count.q.candidates);
    need(count.q.experiments<=std::vector<Experiment>{}.max_size() && count.q.workers<=std::vector<Worker>{}.max_size() &&
        count.q.variants<=std::vector<Variant>{}.max_size() && count.q.nodes<=std::vector<Node>{}.max_size() &&
        count.q.set_members<=NodeSet{}.max_size() && count.q.mappings<=std::vector<Mapping>{}.max_size() &&
        count.q.candidates<=std::vector<std::string>{}.max_size() && identities<=std::vector<Identity>{}.max_size() &&
        names<=std::vector<Name>{}.max_size() && count.q.maximum_cpu_members<=std::vector<std::array<std::uint32_t,2>>{}.max_size() &&
        count.q.range_components<=std::vector<std::array<std::uint32_t,2>>{}.max_size() &&
        count.q.maximum_text<=std::string{}.max_size() && count.q.cpu_input_bytes<=std::string{}.max_size() &&
        count.q.canonical_bytes<=std::string{}.max_size() && count.dynamic<=static_cast<std::size_t>(PTRDIFF_MAX),Error::capacity);
    count.q.retained_bytes=sizeof(Owner)+sizeof(State); add(count.q.retained_bytes,count.dynamic); add(count.q.retained_bytes,count.q.canonical_bytes);
    auto& total=count.q.consumer_simultaneous_bytes; total=decoding?detail::decode_fixed_stock():detail::capture_fixed_stock();
    add(total,count.q.retained_bytes); add(total,count.q.staged_bytes);
    add(total,mul(identities,sizeof(Identity))); add(total,mul(names,sizeof(Name)));
    if (decoding) {
        add(total,count.q.maximum_text); add(total,mul(count.q.range_components,4*sizeof(std::uint32_t)));
        const auto range_text_bound=count.q.range_components?mul(count.q.range_components,22)-1:0;
        need(range_text_bound<=std::string{}.max_size(),Error::capacity); add(total,range_text_bound);
        add(total,10); add(total,detail::factory_error_stock());
        add(total,count.q.cpu_input_bytes); add(total,mul(count.q.maximum_cpu_members,4*sizeof(std::uint32_t)));
    }
    count.limits=&limits; count.check(); return count.q;
}
} // namespace

// Concrete call-cell family union; conversion operands and nested references are distinct.
namespace {
struct PrimitiveCells {
    bool need_condition; Error need_error;
    std::size_t *add_target,*checked_target; std::size_t add_value,checked_value;
    std::size_t mul_a,mul_b,checked_a,checked_b; std::optional<std::size_t> mul_result;
    const Decimal* decimal_self; std::uint64_t decimal_value;
    std::to_chars_result decimal_conversion;
    char nibble_value; unsigned nibble_result,nibble_second_result;
    const Text *text_size_self,*text_at_self,*text_owning_self;
    std::size_t text_at_index,text_loop; std::string text_owned_return;
    const Counts* count_check_self; std::size_t Usage::* count_field;
    std::size_t Usage::* const *count_begin,*count_end;
    std::string_view::const_iterator text_begin,text_end;
    std::size_t text_size_result; char text_at_result;
    Counts *row_self,*text_self,*finish_self;
    std::size_t *row_counter,row_bytes; bool row_staged,text_core; Text text_parameter;
    std::size_t components,text_index;
    Sink *raw_self,*character_self,*number_self,*boolean_self,*text_sink_self;
    std::string_view raw_value,sink_text_value; char character_value,text_octet;
    std::uint64_t number_value; bool boolean_value,sink_core;
    Counts* sink_count; unsigned char text_byte; Decimal sink_decimal;
    std::string_view decimal_view_return,stable_value_return;
    Text counting_text_argument; Counts* cpu_count_self; std::uint32_t cpu_count_value;
    std::optional<std::uint32_t>* cpu_count_previous; std::size_t *cpu_count_members,*cpu_count_input;
    Decimal cpu_count_decimal; Counts* cpu_finish_self; std::size_t cpu_finish_members,cpu_finish_input;
};
template<class P> struct ScalarEmitCells {
    Sink* out; const P* value; Counts* count;
    const core::ByteCount* page_this; std::uint64_t page_result;
    const scenario::ExactRatio *numerator_this,*denominator_this;
    std::uint64_t numerator_result,denominator_result;
};
// Logical reference cells for the inherited parse_error callback; not its ABI layout.
struct ParseErrorCaptureCells {
    topology::detail::StableIdKind* kind;
    std::string* classification;
};
struct FactoryCells {
    ParseErrorCaptureCells parse_error_captures;
    ParseErrorCaptureCells* json_object_fill_reference;
    const ParseErrorCaptureCells* parse_error_callback_this;
    Text numa_parameter,pu_parameter,core_parameter;
    std::string numa_input,pu_input,core_input;
    std::expected<topology::StableNumaNodeId,core::Error> numa;
    std::expected<topology::StableProcessingUnitId,core::Error> pu;
    std::expected<topology::StableCoreId,core::Error> core;
    core::detail::ExpectedErrorCleanup<decltype(numa)> numa_guard;
    core::detail::ExpectedErrorCleanup<decltype(pu)> pu_guard;
    core::detail::ExpectedErrorCleanup<decltype(core)> core_guard;
    decltype(numa_guard)* numa_guard_this; decltype(pu_guard)* pu_guard_this; decltype(core_guard)* core_guard_this;
    topology::StableNumaNodeId numa_return; topology::StableProcessingUnitId pu_return;
    topology::StableCoreId core_return;
    std::string_view numa_parse_input,pu_parse_input,core_parse_input,stable_text;
    topology::detail::StableIdKind stable_kind,valid_kind,kind_name_argument,parse_error_kind;
    std::expected<std::string,core::Error> stable_parsed;
    std::string stable_result,stable_constructor_value,numa_constructor_value,pu_constructor_value,core_constructor_value;
    std::unexpected<core::Error> stable_unexpected,wrapper_unexpected;
    std::string_view valid_text,decimal_text,package_text,range_text;
    std::size_t core_separator; std::uint32_t decimal_value; std::from_chars_result decimal_result;
    bool valid_result,decimal_valid,range_valid,package_valid;
    std::expected<core::CpuSet,core::IdSetParseError> parsed_range;
    core::CpuSet range_owner,range_return;
    std::vector<std::array<std::uint32_t,2>> input_ranges,normalized_ranges;
    std::array<std::uint32_t,2> range_push; const std::array<std::uint32_t,2>* range_current;
    std::size_t component_start,component_end,comma,dash,parse_first,parse_last;
    std::uint32_t range_first,range_last,parse_value;
    std::expected<std::uint32_t,core::IdSetParseError> singleton,first,last;
    std::string_view parse_text; const char *parse_begin,*parse_end;
    std::from_chars_result parse_conversion;
    std::string range_canonical,range_decimal;
    std::vector<std::array<std::uint32_t,2>>::const_iterator range_begin,range_end;
    std::vector<std::array<std::uint32_t,2>>::iterator normalize_begin,normalize_end;
    std::array<std::uint32_t,2>* normalize_current; bool adjacent;
    core::Error error_result; std::string error_classification_owned,error_key;
    std::string_view error_kind,error_classification,kind_name_result;
    nlohmann::json error_object,error_child,error_return,json_assignment_parameter;
    core::detail::JsonCleanup<nlohmann::json> error_guard,child_guard;
    nlohmann::json::object_t* error_entries;
    std::pair<nlohmann::json::object_t::iterator,bool> error_insertion;
    nlohmann::json *retire_value,*retire_child,*set_object,*set_value,*append_object,*append_child,*fill_argument;
    nlohmann::json::iterator retire_begin,retire_end,set_lookup;
    core::detail::JsonCleanup<nlohmann::json>* json_cleanup_this;
    std::string_view set_key,append_name;
};
struct EmitCells {
    Sink *out,*worker_out,*node_out,*placement_out,*ratio_out;
    Counts *count,*worker_count,*node_count,*placement_count;
    const std::vector<Experiment>* records; const Experiment* record;
    const Worker *worker,*worker_argument; const Node *node,*node_argument;
    const Variant* variant; const Mapping* mapping; const std::string* candidate;
    const scenario::ComparisonMemoryPlacement* placement_argument;
    const Common* common; const PerWorker* per; const scenario::ExactRatio* ratio_argument;
    bool first_record,first,placement_first;
    std::vector<Experiment>::const_iterator experiment_begin,experiment_end;
    std::vector<Worker>::const_iterator worker_begin,worker_end;
    std::vector<Variant>::const_iterator variant_begin,variant_end;
    std::vector<Node>::const_iterator node_begin,node_end;
    std::vector<Mapping>::const_iterator mapping_begin,mapping_end;
    std::vector<std::string>::const_iterator candidate_begin,candidate_end;
    NodeSet::const_iterator member_begin,member_end;
    core::CpuSet::const_iterator cpu_begin,cpu_end;
    core::LinuxNumaNodeId member; core::LinuxCpuId cpu;
    std::optional<std::uint32_t> previous; std::size_t members,input;
    const core::CpuSet *size_this,*begin_this,*end_this; std::uint64_t size_result;
    const topology::StableNumaNodeId* node_id_this;
    const topology::StableProcessingUnitId* pu_id_this; const topology::StableCoreId* core_id_this;
    const core::LinuxNumaNodeId* node_linux_this; const core::LinuxCpuId* cpu_this;
    std::uint32_t linux_result,cpu_result;
    std::string_view node_id_result,pu_id_result,core_id_result;
    const scenario::ExactRatio *numerator_this,*denominator_this; std::uint64_t numerator_result,denominator_result;
    scenario::MatrixComparisonOrder order_argument; std::string_view order_return;
    ScalarEmitCells<Common> common_scalars; ScalarEmitCells<PerWorker> per_worker_scalars;
};
struct ParserCells {
    Parser *token_self,*take_self,*number_self,*text_self,*boolean_self,*next_self,*ratio_self,
        *worker_self,*node_self,*mapping_self,*scalars_self,*placement_self,*experiment_self,*run_self;
    std::string_view token_value,take_value,token_suffix,take_suffix;
    std::uint64_t number_max,number_value; std::size_t number_begin,text_begin;
    std::from_chars_result number_conversion; bool text_core; Text text_value;
    bool *next_first; bool run_first,experiment_first,placement_first,common,inherited;
    Ratio ratio_return,practical,confidence,typed_ratio_parameter;
    std::uint64_t gcd_argument_a,gcd_argument_b,gcd_result;
    Text worker_pu,worker_core,node_id,id,workload,variant_name,baseline,candidate;
    std::uint64_t worker_cpu,node_linux;
    std::optional<Worker> worker_result,mapping_worker,experiment_worker;
    std::optional<Node> node_result,mapping_node,placement_node;
    std::optional<Mapping> mapping_result,placement_mapping;
    Worker converting_worker; Node converting_node; Mapping converting_mapping;
    core::LinuxCpuId converting_cpu; core::LinuxNumaNodeId converting_linux_node;
    std::uint32_t cpu_constructor_argument,node_constructor_argument;
    Scalars scalars_return,placement_scalars;
    std::optional<scenario::ComparisonMemoryPlacement> placement_return,experiment_placement;
    Common converting_common; PerWorker converting_per_worker;
    scenario::ComparisonMemoryPlacement converting_placement;
    std::vector<Node> nodes; NodeSet node_members; std::vector<Mapping> mappings;
    std::optional<std::uint32_t> node_previous,cpu_previous; std::uint32_t node_member,cpu_value;
    std::pair<NodeSet::iterator,bool> node_insert_result;
    std::size_t index,variant_index,members,input;
    std::vector<Worker> workers; core::CpuSet cpus;
    std::string cpu_text,cpu_text_swap; std::expected<core::CpuSet,core::IdSetParseError> cpu_parsed;
    Decimal cpu_decimal; std::string_view cpu_parse_input;
    std::vector<Variant> variants; Variant converting_variant;
    std::vector<std::string> candidates;
    scenario::MatrixComparisonOrder order; std::optional<std::uint64_t> seed;
    std::uint64_t seed_conversion; std::optional<Text> pointer; Text pointer_conversion;
    std::optional<std::string> owned_pointer; std::string owned_pointer_conversion;
    Experiment converting_experiment; scenario::MatrixComparisonConfiguration converting_configuration;
    scenario::ValueOrigin converting_origin;
    std::expected<scenario::ExactRatio,core::Error> typed_ratio_result;
    core::detail::ExpectedErrorCleanup<decltype(typed_ratio_result)> ratio_cleanup;
    decltype(ratio_cleanup)* ratio_cleanup_this;
    scenario::ExactRatio typed_ratio_return,practical_return,confidence_return;
    std::uint64_t ratio_factory_numerator,ratio_factory_denominator,ratio_factory_divisor;
    core::ByteCount byte_return; std::uint64_t byte_factory_argument,byte_constructor_argument;
};
template<class P> struct ScalarValidateCells {
    const P* value; std::optional<std::uint64_t>* page;
    std::size_t experiment,variant;
    const core::ByteCount* page_this; std::uint64_t page_result,page_optional_conversion;
};
struct ValidationCells {
    const State* state; std::vector<Identity> identities; std::vector<Name> names;
    std::optional<std::size_t> previous; std::optional<std::uint64_t> page;
    const Experiment* record; const Worker* worker; const Variant* variant;
    const Node* node; const Mapping* mapping; const std::string* candidate;
    const scenario::ValueOrigin* origin; const PerWorker *first_mapping,*per; const Common* common;
    std::size_t variant_index,mapping_index,name_index,identity_index;
    std::vector<Experiment>::const_iterator experiment_begin,experiment_end;
    std::vector<Worker>::const_iterator worker_begin,worker_end;
    std::vector<Variant>::const_iterator variant_begin,variant_end;
    std::vector<Node>::const_iterator node_begin,node_end;
    std::vector<Mapping>::const_iterator mapping_begin,mapping_end;
    std::vector<std::string>::const_iterator candidate_begin,candidate_end;
    NodeSet remaining; const NodeSet* set_copy_argument; NodeSet* erase_this;
    const core::LinuxNumaNodeId* erase_argument; NodeSet::size_type erase_result;
    Name name_push; Identity identity_push;
    const Name *name_a,*name_b; const Identity *current,*previous_identity; const Node* remembered_node;
    IdentityLess identity_callback; const IdentityLess* identity_callback_this; const Identity *identity_a,*identity_b;
    NameLess name_callback; const NameLess* name_callback_this; const Name *callback_name_a,*callback_name_b;
    std::vector<Name>::iterator sort_name_begin,sort_name_end;
    std::vector<Identity>::iterator sort_identity_begin,sort_identity_end;
    std::string_view identifier_argument; char identifier_char;
    std::string_view::const_iterator identifier_begin,identifier_end;
    std::string_view suffix_text,suffix_prefix; std::uint32_t suffix_number; Decimal suffix_decimal;
    std::string_view pointer_value; std::size_t pointer_experiment; std::optional<std::size_t> pointer_variant;
    std::size_t pointer_variant_conversion; Decimal pointer_decimal,subindex_decimal;
    std::string_view origin_pointer_value; std::size_t origin_pointer_experiment; Decimal origin_pointer_decimal;
    std::string_view core_id,component,initial,final; std::uint32_t core_cpu,range_first,range_last;
    std::size_t separator,comma,dash; bool found; std::from_chars_result initial_conversion,final_conversion;
    const Worker* validate_worker_argument; const Node* validate_node_argument;
    const topology::StableNumaNodeId* node_id_this; const topology::StableProcessingUnitId* pu_id_this;
    const topology::StableCoreId* core_id_this; const core::LinuxNumaNodeId* node_linux_this;
    const core::LinuxCpuId* cpu_this; std::uint32_t linux_result,cpu_result;
    std::string_view node_id_result,pu_id_result,core_id_result;
    const core::CpuSet *cpu_size_this,*cpu_contains_this; core::LinuxCpuId cpu_contains_argument;
    std::uint64_t cpu_size_result; bool cpu_contains_result;
    const scenario::ExactRatio *origin_numerator_this,*origin_denominator_this;
    std::uint64_t origin_numerator_result,origin_denominator_result;
    ScalarValidateCells<Common> common_scalars; ScalarValidateCells<PerWorker> worker_scalars;
};
struct OperationCells {
    Limits limits; Counts counted,rendered,admit_count; Usage usage;
    const Limits* admit_limits; bool decoding; std::size_t identities,names;
    std::size_t* simultaneous; std::unique_ptr<State> state; Sink measuring,emitting; std::string_view input;
    Owner converting_owner; Owner* owner_constructor_this; std::unique_ptr<State> owner_parameter;
    Error caught; std::unexpected<Error> error_return,capacity_return; core::Sha256Digest hash_return;
    std::vector<Experiment> experiment_capacity; std::vector<Worker> worker_capacity;
    std::vector<Variant> variant_capacity; std::vector<Node> node_capacity; NodeSet set_capacity;
    std::vector<Mapping> mapping_capacity; std::vector<std::string> candidate_capacity;
    std::vector<Identity> identity_capacity; std::vector<Name> name_capacity; std::string text_capacity;
    std::vector<std::array<std::uint32_t,2>> range_capacity; std::size_t range_text_bound;
};
struct ProbeCells {
    const scenario::ScenarioDigestedPlan* plan;
    std::string_view input;
    Limits limits;
    std::expected<Usage,Error> result;
    const Usage* result_value;
    Error result_error;
    std::unexpected<Error> error_return;
};
struct CaptureCells {
    const scenario::ScenarioDigestedPlan *plan,*canonical_this,*resolved_this,*matrix_this;
    std::string_view canonical_result,resolved_result;
    const scenario::MatrixExpandedScenario *matrix,*comparison_this;
    const std::vector<Experiment> *records,*comparison_result;
};
} // namespace
namespace detail {
auto capture_fixed_stock() noexcept -> std::size_t {
    return sizeof(ProbeCells)+sizeof(PrimitiveCells)+sizeof(EmitCells)+sizeof(ValidationCells)+sizeof(OperationCells)+sizeof(CaptureCells);
}
auto decode_fixed_stock() noexcept -> std::size_t {
    return sizeof(ProbeCells)+sizeof(PrimitiveCells)+sizeof(EmitCells)+sizeof(ValidationCells)+sizeof(OperationCells)+
        sizeof(ParserCells)+sizeof(FactoryCells)+2*sizeof(Parser);
}
auto state_stock() noexcept -> std::size_t { return sizeof(State); }
auto identity_stock() noexcept -> std::size_t { return sizeof(Identity); }
auto name_stock() noexcept -> std::size_t { return sizeof(Name); }
auto factory_error_stock() noexcept -> std::size_t {
    return sizeof(nlohmann::json::object_t)+2*sizeof(nlohmann::json::object_t::value_type)+2*sizeof(nlohmann::json::string_t)+
        std::string_view{"topology.stable_id.invalid_canonical_component"}.size()+
        std::string_view{"Stable topology identity text is malformed or noncanonical"}.size()+
        std::string_view{"parse_stable_topology_id"}.size()+std::string_view{"object_type"}.size()+
        std::string_view{"classification"}.size()+std::string_view{"processing_unit"}.size()+
        std::string_view{"malformed_or_noncanonical"}.size();
}
} // namespace detail
Owner::Owner(std::unique_ptr<State> state) noexcept:state_{std::move(state)} {}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&& other) noexcept -> Owner& { if (this!=&other) state_=std::move(other.state_); return *this; }
Owner::~Owner()=default;
auto Owner::experiments() const noexcept -> const std::vector<Experiment>* { return state_?&state_->experiments:nullptr; }
auto Owner::expansion_profile() const noexcept -> std::string_view { return state_?scenario::matrix_expansion_profile:std::string_view{}; }
auto Owner::canonical() const noexcept -> std::string_view { return state_?std::string_view{state_->canonical}:std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_?&*state_->digest:nullptr; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_?std::optional{state_->origin}:std::nullopt; }
auto Owner::usage() const noexcept -> Usage { return state_?state_->usage:Usage{}; }
auto measure_capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& records=plan.matrix_expanded().comparison_experiments(); Counts counted; counted.limits=&limits; Sink measuring{nullptr,0,limits.canonical_bytes};
        emit(measuring,records,counted); const auto usage=admit(counted,limits,false);
        return usage;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto measure_decode(std::string_view input,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(input.size()<=limits.canonical_bytes,Error::capacity);
        Parser preflight{input,0,{},nullptr}; preflight.count.limits=&limits; preflight.run();
        const auto usage=admit(preflight.count,limits,true); return usage;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Owner,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& records=plan.matrix_expanded().comparison_experiments(); const auto probe=measure_capture(plan,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe;
        auto state=std::make_unique<State>(); state->usage=usage; state->experiments=records; validate(*state);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink out{&state->canonical}; emit(out,state->experiments,rendered);
        state->digest=core::sha256(state->canonical); state->origin=Origin::plan_captured; return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view input,Limits limits) -> std::expected<Owner,Error> {
    try {
        const auto probe=measure_decode(input,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe; auto state=std::make_unique<State>(); state->usage=usage;
        Parser parser{input,0,{},state.get()}; parser.run(); validate(*state);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink out{&state->canonical}; emit(out,state->experiments,rendered);
        need(state->canonical==input,Error::encoding); state->digest=core::sha256(state->canonical);
        state->origin=Origin::decoded_unassociated; return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
} // namespace epycbench::results::comparison_experiments_serialization
