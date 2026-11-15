#include "results/private_kernel_dispatch_serialization.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace epycbench::results::kernel_dispatch_serialization {
using Workload=scenario::DispatchedWorkload;
using Binding=scenario::ExperimentWorkloadBinding;
using Constraint=scenario::DeferredKernelConstraint;
using SeedOrigin=scenario::PointerCycleSeedOrigin;
using Intent=kernel::PointerCycleSeedIntent;
using Mode=kernel::PointerCycleSeedMode;
using Bandwidth=kernel::SequentialReadBandwidthConfig;
using Latency=kernel::DependentLoadLatencyConfig;
using Configuration=kernel::KernelConfiguration;
namespace detail {
struct State {
    Workloads workloads; Bindings bindings; std::string canonical;
    std::optional<core::Sha256Digest> digest; Origin origin{}; Usage usage;
};
auto checked_add(std::size_t& a,std::size_t b) noexcept -> bool {
    if (b>SIZE_MAX-a) return false;
    a+=b; return true;
}
auto checked_multiply(std::size_t a,std::size_t b) noexcept -> std::optional<std::size_t> {
    if (a && b>SIZE_MAX/a) return std::nullopt;
    return a*b;
}
}
namespace {
using detail::State;
constexpr std::array quantities{&Usage::workloads,&Usage::bindings,&Usage::constraints,&Usage::text_bytes,
    &Usage::maximum_text,&Usage::canonical_bytes,&Usage::staged_bytes,&Usage::retained_bytes,&Usage::consumer_simultaneous_bytes};
constexpr std::string_view digits="0123456789abcdef";
void need(bool condition,Error error=Error::schema) { if (!condition) throw error; }
void add(std::size_t& a,std::size_t b) { need(detail::checked_add(a,b),Error::capacity); }
struct Decimal {
    std::array<char,20> data{}; std::size_t length{};
    explicit Decimal(std::uint64_t value) {
        const auto result=std::to_chars(data.data(),data.data()+data.size(),value);
        length=static_cast<std::size_t>(result.ptr-data.data());
    }
    auto view() const noexcept -> std::string_view { return {data.data(),length}; }
};
auto nibble(char c) -> unsigned {
    if (c>='0' && c<='9') return static_cast<unsigned>(c-'0');
    need(c>='a' && c<='f',Error::encoding); return static_cast<unsigned>(c-'a')+10U;
}
struct Text {
    std::string_view bytes; bool encoded{};
    auto size() const noexcept -> std::size_t { return encoded?bytes.size()/2:bytes.size(); }
    auto at(std::size_t i) const -> char {
        return encoded?static_cast<char>((nibble(bytes[2*i])<<4U)|nibble(bytes[2*i+1])):bytes[i];
    }
    auto owning() const -> std::string {
        std::string out; out.reserve(size());
        for (std::size_t i=0;i<size();++i) out.push_back(at(i));
        return out;
    }
};
auto equal(Text a,Text b) -> bool {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i) if (a.at(i)!=b.at(i)) return false;
    return true;
}
auto less(Text a,Text b) -> bool {
    for (std::size_t i=0;i<std::min(a.size(),b.size());++i)
        if (a.at(i)!=b.at(i)) return static_cast<unsigned char>(a.at(i))<static_cast<unsigned char>(b.at(i));
    return a.size()<b.size();
}
void identifier(Text value) {
    need(value.size()>0 && value.size()<=64,Error::reference);
    for (std::size_t i=0;i<value.size();++i) {
        const char c=value.at(i);
        need((c>='a' && c<='z') || (i>0 && ((c>='0' && c<='9') || c=='-')),Error::reference);
    }
}
auto pointer(Text value,std::string_view prefix,Text suffix,std::string_view end={}) -> bool {
    if (value.size()!=prefix.size()+suffix.size()+end.size()) return false;
    for (std::size_t i=0;i<value.size();++i) {
        const char expected=i<prefix.size()?prefix[i]:
            (i<prefix.size()+suffix.size()?suffix.at(i-prefix.size()):end[i-prefix.size()-suffix.size()]);
        if (value.at(i)!=expected) return false;
    }
    return true;
}
void domain(bool latency,std::uint64_t size) {
    need(size>0 && size%64==0 && (!latency || size>=128),Error::reference);
}
struct Counts {
    Usage q; std::size_t dynamic{},stage{}; const Limits* limits{};
    void check() const {
        if (limits) for (const auto field:quantities) need(q.*field<=(*limits).*field,Error::capacity);
    }
    void row(std::size_t& number,std::size_t size) { add(number,1); add(dynamic,size); check(); }
    void text(Text value) {
        add(q.text_bytes,value.size()); add(dynamic,value.size()); add(stage,value.size());
        q.maximum_text=std::max(q.maximum_text,value.size()); check();
    }
    void constraint() { row(q.constraints,sizeof(Constraint)); add(stage,sizeof(Constraint)); }
    void finish() { q.staged_bytes=std::max(q.staged_bytes,stage); stage=0; check(); }
};
struct Sink {
    std::string* output{}; std::size_t size{}; std::size_t ceiling{SIZE_MAX};
    void raw(std::string_view value) { add(size,value.size()); need(size<=ceiling,Error::capacity); if (output) output->append(value); }
    void character(char c) { add(size,1); need(size<=ceiling,Error::capacity); if (output) output->push_back(c); }
    void number(std::uint64_t value) { const Decimal decimal{value}; character('"'); raw(decimal.view()); character('"'); }
    void text(std::string_view value,Counts& count) {
        need(size<=ceiling && ceiling-size>=2 && value.size()<=(ceiling-size-2)/2,Error::capacity);
        count.text({value,false}); character('"');
        for (const char c:value) { const auto b=static_cast<unsigned char>(c); character(digits[b>>4U]); character(digits[b&15U]); }
        character('"');
    }
};
auto constraint_token(Constraint value) -> std::string_view {
    switch (value) {
    case Constraint::working_set_at_least_resolved_page_size: return "\"working_set_at_least_resolved_page_size\"";
    case Constraint::exactly_one_worker: return "\"exactly_one_worker\"";
    }
    throw Error::reference;
}
void emit_configuration(Sink& out,const Configuration& value) {
    if (const auto* b=std::get_if<Bandwidth>(&value)) {
        domain(false,b->working_set_per_worker().value()); out.raw("[\"bandwidth\","); out.number(b->working_set_per_worker().value()); out.character(']');
    } else {
        const auto& l=std::get<Latency>(value); const auto& intent=l.pointer_cycle_seed();
        domain(true,l.working_set().value()); out.raw("[\"latency\","); out.number(l.working_set().value());
        if (intent.mode()==Mode::explicit_value) { need(intent.value().has_value(),Error::reference); out.raw(",[\"explicit\","); out.number(*intent.value()); }
        else { need(intent.mode()==Mode::derive_per_experiment && !intent.value(),Error::reference); out.raw(",[\"derived\",null"); }
        out.raw("]]");
    }
}
void emit_binding(Sink& out,const Binding& value,Counts& count) {
    count.row(count.q.bindings,sizeof(Binding)); out.character('['); out.number(value.experiment_index); out.character(',');
    out.text(value.workload_id,count); out.character(',');
    if (value.pointer_cycle_seed) out.number(*value.pointer_cycle_seed); else out.raw("null");
    out.character(',');
    if (!value.pointer_cycle_seed_origin) out.raw("null");
    else if (*value.pointer_cycle_seed_origin==SeedOrigin::explicit_value) out.raw("\"explicit\"");
    else { need(*value.pointer_cycle_seed_origin==SeedOrigin::derived_value,Error::reference); out.raw("\"derived\""); }
    out.raw(",["); bool first=true;
    for (const auto c:value.deferred_constraints) { if (!first) out.character(','); first=false; count.constraint(); out.raw(constraint_token(c)); }
    out.raw("],"); out.text(value.source_pointer,count); out.character(']'); count.finish();
}
void emit_workload(Sink& out,const Workloads::value_type& entry,Counts& count) {
    const auto& value=entry.second; count.row(count.q.workloads,sizeof(Workloads::value_type));
    out.character('['); out.text(entry.first,count); out.character(','); out.text(value.workload_id,count); out.raw(",\"");
    out.raw(kernel::kernel_id_token(value.kernel_id)); out.raw("\","); out.text(value.kernel_token,count); out.character(',');
    emit_configuration(out,value.configuration); out.character(','); out.text(value.source_pointer,count); out.character(']'); count.finish();
}
void emit(Sink& out,const Workloads& workloads,const Bindings& bindings,Counts& count) {
    need(!count.limits || (workloads.size()<=count.limits->workloads && bindings.size()<=count.limits->bindings),Error::capacity);
    need(!workloads.empty() && !bindings.empty(),Error::reference);
    out.raw("{\"dispatch_profile\":\"epycbench.kernel-dispatch.v1\",\"experiment_workloads\":["); bool first=true;
    for (const auto& binding:bindings) { if (!first) out.character(','); first=false; emit_binding(out,binding,count); }
    out.raw("],\"pointer_cycle_seed_profile\":\"epycbench.pointer-cycle-seed.v1\",\"schema\":\"epycbench.private-kernel-dispatch-artifact/v1\",\"workloads\":["); first=true;
    for (const auto& workload:workloads) { if (!first) out.character(','); first=false; emit_workload(out,workload,count); }
    out.raw("]}"); count.q.canonical_bytes=out.size; count.check();
}
struct ConfigScalars { bool latency{},explicit_seed{}; std::uint64_t bytes{},seed{}; };
auto configuration(ConfigScalars value) -> Configuration {
    const auto bytes=core::ByteCount::from_base_units(value.bytes);
    if (!value.latency) {
        auto result=Bandwidth::create(bytes); need(result.has_value(),Error::typed_factory_failure); return *result;
    }
    const auto intent=value.explicit_seed?Intent::explicit_value(value.seed):Intent::derive_per_experiment();
    auto result=Latency::create(bytes,intent); need(result.has_value(),Error::typed_factory_failure); return *result;
}
struct Parser {
    std::string_view body; std::size_t pos{}; Counts count; State* output{};
    auto take(std::string_view value) -> bool {
        if (!body.substr(pos).starts_with(value)) return false;
        pos+=value.size(); return true;
    }
    void token(std::string_view value) { need(take(value)); }
    auto number(std::uint64_t maximum=UINT64_MAX) -> std::uint64_t {
        token("\""); const auto begin=pos; while (pos<body.size() && body[pos]>='0' && body[pos]<='9') ++pos;
        need(pos>begin && (pos-begin==1 || body[begin]!='0'),Error::encoding); std::uint64_t value{};
        const auto result=std::from_chars(body.data()+begin,body.data()+pos,value);
        need(result.ec==std::errc{} && value<=maximum,Error::encoding); token("\""); return value;
    }
    auto text() -> Text {
        token("\""); const auto begin=pos;
        while (pos<body.size() && body[pos]!='"') { static_cast<void>(nibble(body[pos])); ++pos; }
        need((pos-begin)%2==0,Error::encoding); const Text value{body.substr(begin,pos-begin),true}; token("\""); count.text(value); return value;
    }
    auto next(bool& first) -> bool { if (take("]")) return false; if (!first) token(","); first=false; return true; }
    auto config() -> ConfigScalars {
        ConfigScalars value; token("["); value.latency=!take("\"bandwidth\""); if (value.latency) token("\"latency\"");
        token(","); value.bytes=number(); domain(value.latency,value.bytes);
        if (value.latency) { token(",["); value.explicit_seed=take("\"explicit\""); if (!value.explicit_seed) token("\"derived\""); token(",");
            if (value.explicit_seed) value.seed=number(); else token("null"); token("]"); }
        token("]"); return value;
    }
    void binding() {
        count.row(count.q.bindings,sizeof(Binding)); token("["); const auto index=static_cast<std::size_t>(number(SIZE_MAX));
        need(index==count.q.bindings-1,Error::reference); token(","); const auto id=text(); identifier(id); token(",");
        std::optional<std::uint64_t> seed; if (!take("null")) seed=number(); token(",");
        std::optional<SeedOrigin> origin;
        if (!take("null")) { if (take("\"explicit\"")) origin=SeedOrigin::explicit_value; else { token("\"derived\""); origin=SeedOrigin::derived_value; } }
        token(",["); std::array<Constraint,2> constraints{}; std::size_t size=0; bool first=true;
        while (next(first)) { need(size<constraints.size(),Error::reference); count.constraint();
            if (take("\"working_set_at_least_resolved_page_size\"")) constraints[size++]=Constraint::working_set_at_least_resolved_page_size;
            else { token("\"exactly_one_worker\""); constraints[size++]=Constraint::exactly_one_worker; } }
        token(","); const auto source=text(); const Decimal decimal{index};
        need(pointer(source,"/experiments/",{decimal.view(),false},"/workload"),Error::reference); token("]");
        if (output) output->bindings.push_back(Binding{index,id.owning(),seed,origin,
            std::vector<Constraint>{constraints.begin(),constraints.begin()+static_cast<std::ptrdiff_t>(size)},source.owning()});
        count.finish();
    }
    auto workload() -> Text {
        count.row(count.q.workloads,sizeof(Workloads::value_type)); token("["); const auto key=text(); identifier(key); token(","); const auto id=text(); identifier(id);
        need(equal(key,id),Error::reference); token(",\""); const auto begin=pos;
        while (pos<body.size() && body[pos]!='"') ++pos;
        const auto kernel_id=kernel::parse_kernel_id(body.substr(begin,pos-begin)); need(kernel_id.has_value()); token("\",");
        const auto kernel_token=text(); need(equal(kernel_token,{kernel::kernel_id_token(*kernel_id),false}),Error::reference); token(","); const auto values=config();
        need(values.latency==(kernel_id->family()==kernel::KernelFamily::latency),Error::reference);
        token(","); const auto source=text(); need(pointer(source,"/workloads/",id),Error::reference); token("]");
        if (output) output->workloads.emplace(key.owning(),Workload{id.owning(),*kernel_id,kernel_token.owning(),configuration(values),source.owning()});
        count.finish(); return key;
    }
    void run() {
        token("{\"dispatch_profile\":\"epycbench.kernel-dispatch.v1\",\"experiment_workloads\":["); bool first=true;
        while (next(first)) binding();
        token(",\"pointer_cycle_seed_profile\":\"epycbench.pointer-cycle-seed.v1\",\"schema\":\"epycbench.private-kernel-dispatch-artifact/v1\",\"workloads\":[");
        first=true; std::optional<Text> previous;
        while (next(first)) { const auto key=workload(); if (previous) need(less(*previous,key),Error::reference); previous=key; }
        token("}"); need(pos==body.size()); need(count.q.workloads>0 && count.q.bindings>0,Error::reference);
        count.q.canonical_bytes=body.size(); count.check();
    }
};
void validate(const State& state) {
    need(!state.workloads.empty() && !state.bindings.empty(),Error::reference);
    for (const auto& entry:state.workloads) {
        const auto& value=entry.second; identifier({entry.first,false}); need(entry.first==value.workload_id,Error::reference);
        need(value.kernel_token==kernel::kernel_id_token(value.kernel_id),Error::reference);
        need((value.kernel_id.family()==kernel::KernelFamily::latency)==std::holds_alternative<Latency>(value.configuration),Error::reference);
        need(pointer({value.source_pointer,false},"/workloads/",{value.workload_id,false}),Error::reference);
    }
    std::size_t index=0;
    for (const auto& binding:state.bindings) {
        need(binding.experiment_index==index,Error::reference); const Decimal decimal{index++};
        need(pointer({binding.source_pointer,false},"/experiments/",{decimal.view(),false},"/workload"),Error::reference);
        const auto found=state.workloads.find(binding.workload_id); need(found!=state.workloads.end(),Error::reference);
        const auto* latency=std::get_if<Latency>(&found->second.configuration); const auto& constraints=binding.deferred_constraints;
        if (!latency) { need(!binding.pointer_cycle_seed && !binding.pointer_cycle_seed_origin,Error::reference);
            need(constraints.empty() || (constraints.size()==1 && constraints[0]==Constraint::working_set_at_least_resolved_page_size),Error::reference); }
        else { need(binding.pointer_cycle_seed.has_value() && binding.pointer_cycle_seed_origin.has_value(),Error::reference);
            const auto& intent=latency->pointer_cycle_seed();
            if (intent.mode()==Mode::explicit_value) need(binding.pointer_cycle_seed_origin==SeedOrigin::explicit_value && binding.pointer_cycle_seed==intent.value(),Error::reference);
            else need(intent.mode()==Mode::derive_per_experiment && binding.pointer_cycle_seed_origin==SeedOrigin::derived_value,Error::reference);
            need(constraints.empty() || (constraints.size()==1 && constraints[0]==Constraint::exactly_one_worker) ||
                (constraints.size()==2 && constraints[0]==Constraint::working_set_at_least_resolved_page_size && constraints[1]==Constraint::exactly_one_worker),Error::reference); }
    }
}
auto admit(Counts count,const Limits& limits,bool decoding) -> Usage {
    need(count.q.workloads<=Workloads{}.max_size() && count.q.bindings<=Bindings{}.max_size() &&
         count.q.constraints<=std::vector<Constraint>{}.max_size() && count.q.maximum_text<=std::string{}.max_size() &&
         count.q.canonical_bytes<=std::string{}.max_size() && count.dynamic<=static_cast<std::size_t>(PTRDIFF_MAX),Error::capacity);
    count.q.retained_bytes=sizeof(Owner)+sizeof(State); add(count.q.retained_bytes,count.dynamic); add(count.q.retained_bytes,count.q.canonical_bytes);
    count.q.consumer_simultaneous_bytes=decoding?detail::decode_fixed_stock():detail::capture_fixed_stock();
    add(count.q.consumer_simultaneous_bytes,count.q.retained_bytes); add(count.q.consumer_simultaneous_bytes,count.q.staged_bytes);
    count.limits=&limits; count.check(); return count.q;
}
// Explicit logical cells of the codec and consumed scalar-leaf operations. Library
// internals and allocator metadata are outside this metric; no stack-size claim.
struct PrimitiveCells {
    bool need_condition; Error need_error; std::size_t *add_target,*checked_target;
    std::size_t add_value,checked_value,checked_a,checked_b; std::optional<std::size_t> multiply_return;
    const Decimal* decimal_self; std::uint64_t decimal_value; std::to_chars_result decimal_conversion;
    char nibble_value; unsigned nibble_return,nibble_second_return;
    const Text *size_self,*at_self,*owning_self; std::size_t at_index,owning_index,size_return;
    char at_return; std::string owning_return;
    Text equal_a,equal_b,less_a,less_b,identifier_value,pointer_value,pointer_suffix;
    std::size_t equal_index,less_index,identifier_index,pointer_index; char identifier_char,pointer_expected;
    std::string_view pointer_prefix,pointer_end; bool domain_latency; std::uint64_t domain_size;
    const Counts* check_self; std::size_t Usage::* field; std::size_t Usage::* const *fields_begin,*fields_end;
    Counts *row_self,*text_self,*constraint_self,*finish_self; std::size_t *row_number,row_size; Text counted_text;
    Sink *raw_self,*character_self,*number_self,*sink_text_self; std::string_view raw_value,sink_text_value;
    char character_value,text_char; unsigned char text_byte; std::uint64_t number_value; Decimal decimal;
    Counts* sink_count; std::string_view::const_iterator text_begin,text_end; std::string_view decimal_view_return;
    Constraint token_value; std::string_view constraint_token_return;
};
struct EmitCells {
    Sink *emit_out,*binding_out,*workload_out,*configuration_out; const Workloads* workloads; const Bindings* bindings;
    Counts *emit_count,*binding_count,*workload_count; bool emit_first,binding_first;
    Bindings::const_iterator bindings_begin,bindings_end; Workloads::const_iterator workloads_begin,workloads_end;
    const Binding *binding_loop,*binding_parameter; const Workloads::value_type *workload_loop,*workload_parameter;
    const Workload* workload_value; const Configuration* configuration_parameter;
    const Bandwidth* bandwidth; const Latency* latency; const Intent* intent;
    const std::vector<Constraint>* constraints_range; std::vector<Constraint>::const_iterator constraint_begin,constraint_end;
    Constraint constraint_value;
    const Bandwidth *bandwidth_size_this_a,*bandwidth_size_this_b; const Latency *latency_size_this_a,*latency_size_this_b,*intent_this;
    core::ByteCount bandwidth_size_a,bandwidth_size_b,latency_size_a,latency_size_b;
    const core::ByteCount *bandwidth_value_this_a,*bandwidth_value_this_b,*latency_value_this_a,*latency_value_this_b;
    std::uint64_t bandwidth_value_a,bandwidth_value_b,latency_value_a,latency_value_b;
    const Intent *mode_this_a,*mode_this_b,*seed_this_a,*seed_this_b,*seed_this_c;
    Mode mode_a,mode_b; std::optional<std::uint64_t> seed_a,seed_b,seed_c;
    kernel::KernelId kernel_token_argument; std::string_view kernel_token_return;
};
struct ParserCells {
    Parser preflight,parser;
    Parser *take_self,*token_self,*number_self,*text_self,*next_self,*config_self,*binding_self,*workload_self,*run_self;
    std::string_view take_value,token_value; std::uint64_t number_maximum,number_value; std::size_t number_begin,text_begin;
    std::from_chars_result number_conversion; Text text_return; bool* next_first;
    ConfigScalars config_local,config_return,workload_config;
    std::size_t binding_index,constraint_size; Text binding_id,binding_source; std::optional<std::uint64_t> binding_seed;
    std::optional<SeedOrigin> binding_origin; std::uint64_t converting_seed; SeedOrigin converting_origin;
    std::array<Constraint,2> constraint_array;
    std::array<Constraint,2>::iterator constraint_range_begin,constraint_range_end;
    bool binding_first,run_first;
    Decimal binding_decimal; Binding converting_binding; std::vector<Constraint> converting_constraints;
    Text workload_key,workload_id,workload_token,workload_source,workload_return,run_key;
    std::size_t kernel_begin; std::optional<kernel::KernelId> kernel_id;
    Workload converting_workload; std::string converting_key;
    std::pair<Workloads::iterator,bool> emplace_return;
    std::optional<Text> previous;
    std::string_view parse_id_argument; std::optional<kernel::KernelId> parse_id_return;
    kernel::KernelId token_argument; std::string_view token_return;
};
struct FactoryCells {
    ConfigScalars configuration_argument; core::ByteCount bytes;
    std::uint64_t byte_factory_argument,byte_constructor_argument; core::ByteCount byte_factory_return;
    core::ByteCount* byte_constructor_this; Bandwidth* bandwidth_constructor_this; Latency* latency_constructor_this; Intent* intent_constructor_this;
    std::expected<Bandwidth,core::Error> bandwidth_result; std::expected<Latency,core::Error> latency_result;
    core::ByteCount bandwidth_create_argument,latency_create_argument,bandwidth_constructor_argument,latency_constructor_argument;
    const core::ByteCount *bandwidth_check_this_a,*bandwidth_check_this_b,*latency_check_this_a,*latency_check_this_b;
    std::uint64_t bandwidth_check_a,bandwidth_check_b,latency_check_a,latency_check_b;
    Bandwidth bandwidth_converting_expected; Latency latency_converting_expected;
    const Bandwidth* bandwidth_variant_argument; const Latency* latency_variant_argument;
    Intent intent,latency_create_intent,latency_constructor_intent;
    std::uint64_t explicit_seed_argument; Mode intent_constructor_mode;
    std::optional<std::uint64_t> intent_constructor_value,explicit_converting_optional;
    Intent explicit_return,derived_return;
};
struct ValidationCells {
    const State* state; Workloads::const_iterator workload_begin,workload_end,found,end_result;
    Bindings::const_iterator binding_begin,binding_end; const Workloads::value_type* entry;
    const Workload* value; const Binding* binding; std::size_t index; Decimal decimal;
    const Latency* latency; const Intent* intent; const std::vector<Constraint>* constraints;
    const Workloads* find_this; const std::string* find_key; const Latency* intent_this;
    const Intent *mode_this_a,*mode_this_b,*value_this; Mode mode_a,mode_b; std::optional<std::uint64_t> intent_value;
    kernel::KernelId token_argument; std::string_view token_return;
};
struct OperationCells {
    Limits limits; std::string_view input; Counts measured,rendered,admit_count;
    const Limits* admit_limits; bool admit_decoding; Usage usage;
    Sink measuring,out; std::unique_ptr<State> state,owner_constructor_argument; Owner converting_owner; Owner* owner_constructor_this;
    Error caught_error; std::unexpected<Error> error_conversion,capacity_conversion;
    const std::length_error* length_error;
    Workloads max_map; Bindings max_bindings; std::vector<Constraint> max_constraints; std::string max_text,max_canonical;
    std::string_view digest_argument; core::Sha256Digest digest_return;
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
    std::string_view canonical_return,resolved_return;
    const scenario::MatrixExpandedScenario *matrix_return,*kernel_this;
    const scenario::KernelDispatchedScenario *kernel_return,*kernel,*workloads_this,*bindings_this,*profile_this;
    const Workloads* workloads_return; const Bindings* bindings_return; std::string_view profile_return;
};
} // namespace
namespace detail {
auto capture_fixed_stock() noexcept -> std::size_t {
    return sizeof(ProbeCells)+sizeof(PrimitiveCells)+sizeof(EmitCells)+sizeof(ValidationCells)+sizeof(OperationCells)+sizeof(CaptureCells);
}
auto decode_fixed_stock() noexcept -> std::size_t {
    return sizeof(ProbeCells)+sizeof(PrimitiveCells)+sizeof(EmitCells)+sizeof(ParserCells)+sizeof(FactoryCells)+sizeof(ValidationCells)+sizeof(OperationCells);
}
auto state_stock() noexcept -> std::size_t { return sizeof(State); }
}
Owner::Owner(std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&& other) noexcept -> Owner& { if (this!=&other) state_=std::move(other.state_); return *this; }
Owner::~Owner()=default;
auto Owner::workloads() const noexcept -> const Workloads* { return state_?&state_->workloads:nullptr; }
auto Owner::experiment_workloads() const noexcept -> const Bindings* { return state_?&state_->bindings:nullptr; }
auto Owner::dispatch_profile() const noexcept -> std::string_view { return state_?scenario::kernel_dispatch_profile:std::string_view{}; }
auto Owner::seed_profile() const noexcept -> std::string_view { return state_?kernel::pointer_cycle_seed_profile:std::string_view{}; }
auto Owner::canonical() const noexcept -> std::string_view { return state_?std::string_view{state_->canonical}:std::string_view{}; }
auto Owner::digest() const noexcept -> const core::Sha256Digest* { return state_?&*state_->digest:nullptr; }
auto Owner::origin() const noexcept -> std::optional<Origin> { return state_?std::optional{state_->origin}:std::nullopt; }
auto Owner::usage() const noexcept -> Usage { return state_?state_->usage:Usage{}; }
auto measure_capture(const scenario::ScenarioDigestedPlan& plan,Limits limits) -> std::expected<Usage,Error> {
    try {
        need(!plan.canonical_scenario_json().empty() && !plan.canonical_resolved_scenario_plan_json().empty(),Error::state);
        const auto& kernel=plan.matrix_expanded().kernel_dispatched();
        need(kernel.dispatch_profile()==scenario::kernel_dispatch_profile,Error::reference);
        Counts measured; measured.limits=&limits; Sink measuring{nullptr,0,limits.canonical_bytes};
        emit(measuring,kernel.workloads(),kernel.experiment_workloads(),measured);
        const auto usage=admit(measured,limits,false); return usage;
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
        const auto& kernel=plan.matrix_expanded().kernel_dispatched();
        need(kernel.dispatch_profile()==scenario::kernel_dispatch_profile,Error::reference);
        const auto probe=measure_capture(plan,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe; auto state=std::make_unique<State>(); state->usage=usage;
        state->workloads=kernel.workloads(); state->bindings=kernel.experiment_workloads(); validate(*state);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink out{&state->canonical};
        emit(out,state->workloads,state->bindings,rendered); state->digest=core::sha256(state->canonical); state->origin=Origin::plan_captured;
        return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
auto Owner::decode(std::string_view input,Limits limits) -> std::expected<Owner,Error> {
    try {
        const auto probe=measure_decode(input,limits);
        if (!probe) return std::unexpected{probe.error()};
        const auto usage=*probe; auto state=std::make_unique<State>(); state->usage=usage;
        Parser parser{input,0,{},state.get()}; parser.run(); validate(*state);
        state->canonical.reserve(usage.canonical_bytes); Counts rendered; Sink out{&state->canonical};
        emit(out,state->workloads,state->bindings,rendered); need(state->canonical==input,Error::encoding);
        state->digest=core::sha256(state->canonical); state->origin=Origin::decoded_unassociated; return Owner{std::move(state)};
    } catch (Error error) { return std::unexpected{error}; }
      catch (const std::length_error&) { return std::unexpected{Error::capacity}; }
}
} // namespace epycbench::results::kernel_dispatch_serialization
