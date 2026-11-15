#include "results/private_memory_placement_serialization.hpp"
#include "core/json_ownership.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace epycbench::results::memory_placement_serialization {
using Comparison = scenario::ComparisonVariantMemoryPlacement;
using Template = scenario::MatrixMemoryPlacementTemplate;
using Common = scenario::ResolvedMemoryPlacement;
using PerWorker = scenario::ResolvedPerWorkerMemoryPlacement;
using Node = scenario::ResolvedMemoryNode;
using Mapping = scenario::ResolvedPerWorkerMemoryNode;
using NodeSet = std::set<core::LinuxNumaNodeId>;
namespace detail {
struct State {
    std::vector<Comparison> comparisons;
    std::vector<Template> templates;
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
    &Usage::comparisons, &Usage::templates, &Usage::nodes, &Usage::set_members,
    &Usage::mappings, &Usage::text_bytes, &Usage::maximum_text, &Usage::range_components,
    &Usage::canonical_bytes, &Usage::staged_bytes, &Usage::retained_bytes,
    &Usage::consumer_simultaneous_bytes};
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
void emit_node(Sink& out,const Node& node,Counts& count) {
    out.character('['); out.text(node.stable_id.value(),count); out.character(',');
    out.number(node.linux_node.value()); out.character(']');
}
void emit_mapping(Sink& out,const Mapping& row,Counts& count) {
    out.raw("[["); out.text(row.worker.stable_id.value(),count); out.character(',');
    out.number(row.worker.linux_cpu.value()); out.character(',');
    out.text(row.worker.core_id.value(),count,true); out.raw("],");
    emit_node(out,row.node,count); out.character(']');
}
template<class Placement> void emit_scalars(Sink& out,const Placement& value,Counts& count) {
    need(value.policy==scenario::MemoryPolicy::bind && value.page_mode==scenario::PageMode::base,Error::reference);
    out.raw(",\"base\","); out.number(value.page_size.value()); out.raw(",[");
    out.number(value.placement_threshold.numerator()); out.character(',');
    out.number(value.placement_threshold.denominator()); out.raw("],");
    out.boolean(value.prefault); out.character(','); out.boolean(value.verify_before);
    out.character(','); out.boolean(value.verify_after); out.character(',');
    out.text(value.source_pointer,count);
}
void emit(Sink& out,const std::vector<Comparison>& comparisons,
          const std::vector<Template>& templates,Counts& count) {
    need(!count.limits || (comparisons.size()<=count.limits->comparisons && templates.size()<=count.limits->templates),Error::capacity);
    out.raw("{\"comparison_memory\":["); bool first=true;
    for (const auto& record:comparisons) {
        count.row(count.q.comparisons,sizeof(Comparison),false);
        if (!first) out.character(',');
        first=false;
        out.character('['); out.number(record.experiment_index); out.character(',');
        out.number(record.variant_index); out.character(',');
        if (const auto* common=std::get_if<Common>(&record.placement)) {
            out.raw("[\"common\",\"bind\",["); bool initial=true;
            for (const auto& node:common->nodes) {
                count.row(count.q.nodes,sizeof(Node),true);
                if (!initial) out.character(',');
                initial=false; emit_node(out,node,count);
            }
            out.raw("],["); initial=true;
            for (const auto member:common->linux_nodes) {
                count.row(count.q.set_members,sizeof(NodeSet::value_type),true);
                if (!initial) out.character(',');
                initial=false; out.number(member.value());
            }
            out.character(']'); emit_scalars(out,*common,count);
        } else {
            const auto& mapping=std::get<PerWorker>(record.placement);
            out.raw("[\"per_worker\",\"bind\",["); bool initial=true;
            for (const auto& row:mapping.worker_nodes) {
                count.row(count.q.mappings,sizeof(Mapping),true);
                if (!initial) out.character(',');
                initial=false; emit_mapping(out,row,count);
            }
            out.character(']'); emit_scalars(out,mapping,count);
        }
        out.raw("]]"); count.finish();
    }
    out.raw("],\"matrix_memory_templates\":["); first=true;
    for (const auto& record:templates) {
        count.row(count.q.templates,sizeof(Template),false);
        if (!first) out.character(',');
        first=false;
        out.character('['); out.number(record.experiment_index); out.raw(",\"bind\"");
        emit_scalars(out,record,count); out.character(','); out.text(record.profile,count);
        out.character(']'); count.finish();
    }
    out.raw("],\"memory_profile\":\"epycbench.memory-placement-resolution.v1\",\"schema\":\"");
    out.raw(profile); out.raw("\"}"); count.q.canonical_bytes=out.size; count.check();
}
template<class Id> auto factory(Text text) -> Id {
    const auto input=text.owning();
    auto value=Id::parse(input);
    core::detail::ExpectedErrorCleanup cleanup{value};
    need(value.has_value(),Error::typed_factory_failure);
    return std::move(*value);
}
struct Scalars {
    std::uint64_t page{},numerator{},denominator{};
    bool prefault{},before{},after{};
    Text pointer;
};
struct Parser {
    std::string_view body;
    std::size_t pos{};
    Counts count;
    State* output{};
    void token(std::string_view value) {
        need(body.substr(pos).starts_with(value)); pos+=value.size();
    }
    auto take(std::string_view value) -> bool {
        if (!body.substr(pos).starts_with(value)) return false;
        pos+=value.size(); return true;
    }
    auto number(std::uint64_t maximum=UINT64_MAX) -> std::uint64_t {
        token("\""); const auto begin=pos;
        while (pos<body.size() && body[pos]>='0' && body[pos]<='9') ++pos;
        need(pos>begin && (pos-begin==1 || body[begin]!='0'),Error::encoding);
        std::uint64_t value{};
        const auto result=std::from_chars(body.data()+begin,body.data()+pos,value);
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
    auto node() -> std::optional<Node> {
        token("["); const auto id=text(); token(","); const auto linux=number(UINT32_MAX); token("]");
        if (!output) return std::nullopt;
        return Node{factory<topology::StableNumaNodeId>(id),core::LinuxNumaNodeId{static_cast<std::uint32_t>(linux)}};
    }
    auto mapping() -> std::optional<Mapping> {
        token("[["); const auto pu=text(); token(","); const auto cpu=number(UINT32_MAX);
        token(","); const auto core=text(true); token("],"); auto target=node(); token("]");
        if (!output) return std::nullopt;
        return Mapping{{factory<topology::StableProcessingUnitId>(pu),
            core::LinuxCpuId{static_cast<std::uint32_t>(cpu)},factory<topology::StableCoreId>(core)},std::move(*target)};
    }
    auto scalars() -> Scalars {
        token(",\"base\","); Scalars value; value.page=number(); token(",[");
        value.numerator=number(); token(","); value.denominator=number(); token("],");
        need(value.denominator && value.numerator<=value.denominator &&
             std::gcd(value.numerator,value.denominator)==1,Error::reference);
        value.prefault=boolean(); token(","); value.before=boolean(); token(",");
        value.after=boolean(); token(","); value.pointer=text(); return value;
    }
    void comparison() {
        count.row(count.q.comparisons,sizeof(Comparison),false);
        token("["); const auto experiment=static_cast<std::size_t>(number(SIZE_MAX)); token(",");
        const auto variant=static_cast<std::size_t>(number(SIZE_MAX)); token(",[");
        const bool common=take("\"common\""); if (!common) token("\"per_worker\"");
        token(",\"bind\",["); bool first=true;
        std::vector<Node> nodes; NodeSet members; std::vector<Mapping> mappings;
        while (next(first)) {
            if (common) {
                count.row(count.q.nodes,sizeof(Node),true); auto value=node();
                if (output) nodes.push_back(std::move(*value));
            } else {
                count.row(count.q.mappings,sizeof(Mapping),true); auto value=mapping();
                if (output) mappings.push_back(std::move(*value));
            }
        }
        if (common) {
            token(",["); first=true; std::optional<std::uint32_t> previous;
            while (next(first)) {
                count.row(count.q.set_members,sizeof(NodeSet::value_type),true);
                const auto value=static_cast<std::uint32_t>(number(UINT32_MAX));
                need(!previous || *previous<value,Error::reference); previous=value;
                if (output) members.emplace(value);
            }
        }
        const auto value=scalars(); token("]]"); count.finish();
        if (output) {
            auto ratio=scenario::ExactRatio::create(value.numerator,value.denominator);
            core::detail::ExpectedErrorCleanup cleanup{ratio};
            need(ratio.has_value(),Error::typed_factory_failure);
            if (common) {
                Common placement{scenario::MemoryPolicy::bind,std::move(nodes),std::move(members),
                    scenario::PageMode::base,core::ByteCount::from_base_units(value.page),*ratio,
                    value.prefault,value.before,value.after,value.pointer.owning()};
                output->comparisons.push_back({experiment,variant,std::move(placement)});
            } else {
                PerWorker placement{scenario::MemoryPolicy::bind,std::move(mappings),scenario::PageMode::base,
                    core::ByteCount::from_base_units(value.page),*ratio,value.prefault,value.before,value.after,
                    value.pointer.owning()};
                output->comparisons.push_back({experiment,variant,std::move(placement)});
            }
        }
    }
    void matrix() {
        count.row(count.q.templates,sizeof(Template),false);
        token("["); const auto experiment=static_cast<std::size_t>(number(SIZE_MAX)); token(",\"bind\"");
        const auto value=scalars(); token(","); const auto retained_profile=text(); token("]"); count.finish();
        if (output) {
            auto ratio=scenario::ExactRatio::create(value.numerator,value.denominator);
            core::detail::ExpectedErrorCleanup cleanup{ratio}; need(ratio.has_value(),Error::typed_factory_failure);
            output->templates.push_back({experiment,scenario::MemoryPolicy::bind,scenario::PageMode::base,
                core::ByteCount::from_base_units(value.page),*ratio,value.prefault,value.before,value.after,
                value.pointer.owning(),retained_profile.owning()});
        }
    }
    void run() {
        token("{\"comparison_memory\":["); bool first=true;
        while (next(first)) comparison();
        token(",\"matrix_memory_templates\":["); first=true;
        while (next(first)) matrix();
        token(",\"memory_profile\":\"epycbench.memory-placement-resolution.v1\",\"schema\":\"");
        token(profile); token("\"}"); need(pos==body.size(),Error::encoding);
        count.q.canonical_bytes=body.size(); count.check();
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
struct Identity { const Mapping* mapping; std::size_t record; };
struct IdentityLess {
    auto operator()(const Identity& a,const Identity& b) const noexcept -> bool {
        if (a.mapping->worker.linux_cpu!=b.mapping->worker.linux_cpu)
            return a.mapping->worker.linux_cpu<b.mapping->worker.linux_cpu;
        return a.record<b.record;
    }
};
void validate_node(const Node& node) {
    need(suffix_id(node.stable_id.value(),"numa/",node.linux_node.value()),Error::reference);
}
template<class Placement> void validate_scalars(const Placement& value,std::optional<std::uint64_t>& page,
                                              std::size_t experiment,std::optional<std::size_t> variant) {
    need(value.page_size.value()>0 && (!page || *page==value.page_size.value()),Error::reference);
    page=value.page_size.value(); need(pointer_valid(value.source_pointer,experiment,variant),Error::reference);
}
void validate(const State& state) {
    std::vector<Identity> index;
    index.reserve(state.usage.mappings);
    std::optional<std::uint64_t> page;
    std::optional<std::size_t> experiment;
    std::size_t variant{},record_index{},template_index{};
    const PerWorker* first_mapping=nullptr;
    for (const auto& record:state.comparisons) {
        if (!experiment || *experiment!=record.experiment_index) {
            need(!experiment || (*experiment<record.experiment_index && variant>=2),Error::reference);
            experiment=record.experiment_index; variant=0; first_mapping=nullptr;
        }
        need(record.variant_index==variant,Error::reference); add(variant,1);
        while (template_index<state.templates.size() && state.templates[template_index].experiment_index<*experiment) ++template_index;
        need(template_index==state.templates.size() || state.templates[template_index].experiment_index!=*experiment,Error::reference);
        if (const auto* common=std::get_if<Common>(&record.placement)) {
            validate_scalars(*common,page,*experiment,record.variant_index);
            need(!common->nodes.empty() && common->nodes.size()==common->linux_nodes.size(),Error::reference);
            // The single temporary numeric set reuses Q after construction staging has retired.
            NodeSet remaining=common->linux_nodes;
            for (const auto& node:common->nodes) { validate_node(node); need(remaining.erase(node.linux_node)==1,Error::reference); }
        } else {
            const auto& mapping=std::get<PerWorker>(record.placement);
            validate_scalars(mapping,page,*experiment,record.variant_index);
            need(!mapping.worker_nodes.empty(),Error::reference);
            if (first_mapping) need(first_mapping->worker_nodes==mapping.worker_nodes,Error::reference);
            else first_mapping=&mapping;
            for (const auto& row:mapping.worker_nodes) {
                validate_node(row.node);
                need(suffix_id(row.worker.stable_id.value(),"pu/",row.worker.linux_cpu.value()) &&
                    core_contains(row.worker.core_id.value(),row.worker.linux_cpu.value()),Error::reference);
                index.push_back({&row,record_index});
            }
        }
        ++record_index;
    }
    need(!experiment || variant>=2,Error::reference);
    experiment.reset();
    for (const auto& item:state.templates) {
        need(!experiment || *experiment<item.experiment_index,Error::reference); experiment=item.experiment_index;
        validate_scalars(item,page,item.experiment_index,std::nullopt);
        need(item.profile==scenario::memory_placement_resolution_profile,Error::reference);
    }
    std::ranges::sort(index,IdentityLess{});
    for (std::size_t i=1;i<index.size();++i) {
        const auto& a=index[i-1]; const auto& b=index[i];
        if (a.mapping->worker.linux_cpu==b.mapping->worker.linux_cpu)
            need(a.record!=b.record && *a.mapping==*b.mapping,Error::reference);
    }
}
auto admit(Counts count,const Limits& limits,bool decoding) -> Usage {
    need(count.q.comparisons<=std::vector<Comparison>{}.max_size() &&
        count.q.templates<=std::vector<Template>{}.max_size() && count.q.nodes<=std::vector<Node>{}.max_size() &&
        count.q.set_members<=NodeSet{}.max_size() && count.q.mappings<=std::vector<Mapping>{}.max_size() &&
        count.q.mappings<=std::vector<Identity>{}.max_size() &&
        count.q.range_components<=std::vector<std::array<std::uint32_t,2>>{}.max_size() && count.q.maximum_text<=std::string{}.max_size() &&
        count.q.canonical_bytes<=std::string{}.max_size() &&
        count.dynamic<=static_cast<std::size_t>(PTRDIFF_MAX),Error::capacity);
    count.q.retained_bytes=sizeof(Owner)+sizeof(State); add(count.q.retained_bytes,count.dynamic);
    add(count.q.retained_bytes,count.q.canonical_bytes);
    auto& total=count.q.consumer_simultaneous_bytes;
    total=decoding?detail::decode_fixed_stock():detail::capture_fixed_stock();
    add(total,count.q.retained_bytes); add(total,count.q.staged_bytes);
    add(total,mul(count.q.mappings,sizeof(Identity)));
    if (decoding) {
        add(total,count.q.maximum_text); add(total,mul(count.q.range_components,4*sizeof(std::uint32_t)));
        const auto range_text_bound=count.q.range_components?mul(count.q.range_components,22)-1:0;
        need(range_text_bound<=std::string{}.max_size(),Error::capacity);
        add(total,range_text_bound);
        add(total,10); add(total,detail::factory_error_stock());
    }
    count.limits=&limits; count.check(); return count.q;
}
} // namespace

// Logical call-cell union: one instance of each concrete frame family, with explicit
// multiplicity for simultaneously nested node/text and scalar template frames.
namespace {
struct PrimitiveCells {
    bool need_condition; Error need_error;
    std::size_t *add_target,*checked_target; std::size_t add_value,checked_value;
    std::size_t mul_a,mul_b,checked_a,checked_b; std::optional<std::size_t> mul_result;
    const Decimal* decimal_self; std::uint64_t decimal_value;
    std::to_chars_result decimal_conversion;
    char nibble_value; unsigned nibble_result;
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
};
template<class P> struct ScalarEmitCells {
    Sink* out; const P* value; Counts* count;
    const core::ByteCount* page_this; std::uint64_t page_result;
    const scenario::ExactRatio *numerator_this,*denominator_this;
    std::uint64_t numerator_result,denominator_result;
};
struct EmitCells {
    Sink *out,*node_out,*mapping_out;
    const std::vector<Comparison>* comparisons; const std::vector<Template>* templates;
    Counts *count,*node_count,*mapping_count;
    bool first,common_initial,worker_initial;
    const Comparison* record; const Template* item; const Node* node;
    const Mapping* mapping; const Common* common; const PerWorker* per_worker;
    const Node* emit_node_argument; const Mapping* emit_mapping_argument;
    std::vector<Comparison>::const_iterator comparison_begin,comparison_end;
    std::vector<Template>::const_iterator template_begin,template_end;
    std::vector<Node>::const_iterator node_begin,node_end;
    std::vector<Mapping>::const_iterator mapping_begin,mapping_end;
    NodeSet::const_iterator member_begin,member_end;
    core::LinuxNumaNodeId member;
    const topology::StableNumaNodeId* node_id_this;
    const topology::StableProcessingUnitId* pu_id_this;
    const topology::StableCoreId* core_id_this;
    const core::LinuxNumaNodeId* node_linux_this; const core::LinuxCpuId* cpu_this;
    std::uint32_t linux_result,cpu_result;
    ScalarEmitCells<Common> common_scalars;
    ScalarEmitCells<PerWorker> per_worker_scalars;
    ScalarEmitCells<Template> template_scalars;
};
struct ParserCells {
    Parser *token_self,*take_self,*number_self,*text_self,*boolean_self,*next_self,
        *node_self,*mapping_self,*scalars_self,*comparison_self,*matrix_self,*run_self;
    std::string_view token_value,take_value;
    std::uint64_t number_max,number_value; std::size_t number_begin,text_begin;
    std::from_chars_result number_conversion; bool text_core; Text text_value;
    bool* next_first; bool run_first,comparison_first,common;
    Text node_id,worker_pu,worker_core; std::uint64_t node_linux,worker_cpu;
    std::optional<Node> node_result,mapping_node,comparison_node;
    std::optional<Mapping> mapping_result,comparison_mapping;
    Node converting_node; Mapping converting_mapping;
    Scalars scalars_return,comparison_scalars,matrix_scalars;
    std::size_t experiment,variant,matrix_experiment;
    std::vector<Node> nodes; NodeSet members; std::vector<Mapping> mappings;
    std::optional<std::uint32_t> previous; std::uint32_t member;
    Common common_placement; PerWorker per_worker_placement;
    Comparison common_push,per_worker_push; Template matrix_push;
    Text matrix_profile;
    std::expected<scenario::ExactRatio,core::Error> comparison_ratio,matrix_ratio;
    core::detail::ExpectedErrorCleanup<decltype(comparison_ratio)> comparison_cleanup,matrix_cleanup;
    decltype(comparison_cleanup) *comparison_cleanup_this,*matrix_cleanup_this;
    std::uint64_t ratio_numerator,ratio_denominator,ratio_divisor;
    scenario::ExactRatio ratio_return; core::ByteCount byte_return;
    std::uint64_t byte_argument;
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
template<class P> struct ScalarValidateCells {
    const P* value; std::optional<std::uint64_t>* page;
    std::size_t experiment; std::optional<std::size_t> variant;
    const core::ByteCount* page_this; std::uint64_t page_result;
};
struct ValidationCells {
    const State* state; std::vector<Identity> index;
    std::optional<std::uint64_t> page; std::optional<std::size_t> experiment;
    std::size_t variant,record_index,template_index,identity_index;
    const PerWorker *first_mapping,*mapping;
    const Common* common; const Comparison* record; const Template* item;
    const Node* node; const Mapping* row; NodeSet remaining;
    const NodeSet* set_copy_argument; NodeSet* erase_this;
    const core::LinuxNumaNodeId* erase_argument; NodeSet::size_type erase_result;
    std::vector<Comparison>::const_iterator comparison_begin,comparison_end;
    std::vector<Template>::const_iterator template_begin,template_end;
    std::vector<Node>::const_iterator node_begin,node_end;
    std::vector<Mapping>::const_iterator mapping_begin,mapping_end;
    const Identity *identity_a,*identity_b;
    Identity push; IdentityLess sort_callback; const IdentityLess* callback_this;
    const Identity *callback_a,*callback_b;
    const core::LinuxCpuId *callback_cpu_a,*callback_cpu_b;
    std::string_view suffix_text,suffix_prefix; std::uint32_t suffix_id;
    Decimal suffix_decimal;
    std::string_view pointer_value; std::size_t pointer_experiment;
    std::optional<std::size_t> pointer_variant; Decimal pointer_decimal,subindex_decimal;
    std::string_view core_id,component,initial,final;
    std::uint32_t core_cpu,range_first,range_last; std::size_t separator,comma,dash;
    bool found; std::from_chars_result initial_conversion,final_conversion;
    const Node* validate_node_argument;
    const topology::StableNumaNodeId* node_id_this;
    const topology::StableProcessingUnitId* worker_id_this;
    const topology::StableCoreId* worker_core_this;
    const core::LinuxNumaNodeId* linux_node_this; const core::LinuxCpuId* linux_cpu_this;
    std::string_view node_id_result,worker_id_result,worker_core_result;
    std::uint32_t linux_node_result,linux_cpu_result;
    ScalarValidateCells<Common> common_scalars;
    ScalarValidateCells<PerWorker> worker_scalars;
    ScalarValidateCells<Template> template_scalars;
};
struct OperationCells {
    Limits limits; Counts counted,rendered,admit_count; Usage usage;
    const Limits* admit_limits; bool decoding;
    std::size_t* simultaneous; std::unique_ptr<State> state;
    Sink measuring,emitting; std::string_view input;
    Owner converting_owner; std::unique_ptr<State> owner_parameter;
    Error caught; std::unexpected<Error> error_return,capacity_return;
    core::Sha256Digest hash_return;
    std::vector<Comparison> comparison_capacity; std::vector<Template> template_capacity;
    std::vector<Node> node_capacity; NodeSet set_capacity; std::vector<Mapping> mapping_capacity;
    std::vector<Identity> identity_capacity; std::string text_capacity;
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
    const scenario::MatrixExpandedScenario *matrix,*kernel_this;
    const scenario::KernelDispatchedScenario *kernel,*memory_this;
    const scenario::MemoryPlacementResolvedScenario *memory,*comparison_this,*template_this;
    const std::vector<Comparison>* comparison_result; const std::vector<Template>* template_result;
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
auto factory_error_stock() noexcept -> std::size_t {
    return sizeof(nlohmann::json::object_t)+2*sizeof(nlohmann::json::object_t::value_type)+
        2*sizeof(nlohmann::json::string_t)+
        std::string_view{"topology.stable_id.invalid_canonical_component"}.size()+
        std::string_view{"Stable topology identity text is malformed or noncanonical"}.size()+
        std::string_view{"parse_stable_topology_id"}.size()+std::string_view{"object_type"}.size()+
        std::string_view{"classification"}.size()+std::string_view{"processing_unit"}.size()+
        std::string_view{"malformed_or_noncanonical"}.size();
}
} // namespace detail
Owner::Owner(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&& other) noexcept -> Owner& {
    if (this!=&other) state_=std::move(other.state_);
    return *this;
}
Owner::~Owner()=default;
auto Owner::comparison_memory() const noexcept -> const std::vector<Comparison>* { return state_?&state_->comparisons:nullptr; }
auto Owner::matrix_memory_templates() const noexcept -> const std::vector<Template>* { return state_?&state_->templates:nullptr; }
auto Owner::memory_profile() const noexcept -> std::string_view { return state_?scenario::memory_placement_resolution_profile:std::string_view{}; }
auto Owner::canonical() const noexcept -> std::string_view { return state_?std::string_view{state_->canonical}:std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_?&*state_->digest:nullptr; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_?std::optional{state_->origin}:std::nullopt; }
auto Owner::usage() const noexcept -> Usage { return state_?state_->usage:Usage{}; }
auto measure_capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& memory=plan.matrix_expanded().kernel_dispatched().memory_placement_resolved();
        Counts counted; counted.limits=&limits; Sink measuring{nullptr,0,limits.canonical_bytes};
        emit(measuring,memory.comparison_memory(),memory.matrix_memory_templates(),counted);
        const auto usage=admit(counted,limits,false);
        return usage;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto measure_decode(std::string_view input,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(input.size()<=limits.canonical_bytes,Error::capacity);
        Parser preflight{input,0,{},nullptr}; preflight.count.limits=&limits; preflight.run();
        const auto usage=admit(preflight.count,limits,true);
        return usage;
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Owner,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& memory=plan.matrix_expanded().kernel_dispatched().memory_placement_resolved();
        const auto probe=measure_capture(plan,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe;
        auto state=std::make_unique<State>(); state->usage=usage;
        state->comparisons=memory.comparison_memory(); state->templates=memory.matrix_memory_templates();
        validate(*state); state->canonical.reserve(usage.canonical_bytes);
        Counts rendered; Sink out{&state->canonical}; emit(out,state->comparisons,state->templates,rendered);
        state->digest=core::sha256(state->canonical); state->origin=Origin::plan_captured;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view input,Limits limits) -> std::expected<Owner,Error> {
    try {
        const auto probe=measure_decode(input,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe;
        auto state=std::make_unique<State>(); state->usage=usage;
        Parser parser{input,0,{},state.get()}; parser.run(); validate(*state);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink out{&state->canonical};
        emit(out,state->comparisons,state->templates,rendered); need(state->canonical==input,Error::encoding);
        state->digest=core::sha256(state->canonical); state->origin=Origin::decoded_unassociated;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
} // namespace epycbench::results::memory_placement_serialization
