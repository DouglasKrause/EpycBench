#include "results/private_implementation_selection.hpp"
#include "epycbench/kernel/registry.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace epycbench::results::implementation_selection {
namespace {
using Z=std::size_t;
using SV=std::string_view;
using Digest=core::Sha256Digest;
namespace ns=normalization_serialization;
using Workloads=kernel_dispatch_serialization::Workloads;
using Bindings=kernel_dispatch_serialization::Bindings;
using State=detail::State;
void need(bool value,Error error) { if(!value)throw error; }
auto add(Z a,Z b) -> Z { auto value=detail::checked_add(a,b);need(value.has_value(),Error::capacity);return *value; }
auto mul(Z a,Z b) -> Z { auto value=detail::checked_multiply(a,b);need(value.has_value(),Error::capacity);return *value; }
void bound(Z value,Z limit) { need(value<=limit,Error::capacity); }
struct Work {
    Z used,limit;
    void debit(Z amount=1) { bound(amount,limit-used);used+=amount; }
    auto equal(SV a,SV b) -> bool { debit(add(1,add(a.size(),b.size())));return a==b; }
    auto less(SV a,SV b) -> bool { debit(add(1,add(a.size(),b.size())));return a<b; }
};
auto safe_token(SV text) noexcept -> bool {
    if(text.empty())return false;
    const char* current=text.data();const char* end=current+text.size();
    while(current!=end) {
        const auto c=static_cast<unsigned char>(*current++);
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='/'||c=='_'))return false;
    }
    return true;
}
auto search(std::span<const Workload> rows,SV key,Work& work) -> Z {
    Z first=0,last=rows.size();
    while(first<last) {
        work.debit();const Z middle=first+(last-first)/2;
        if(work.less(rows[middle].workload_id,key))first=middle+1;
        else last=middle;
    }
    need(first<rows.size()&&work.equal(rows[first].workload_id,key),Error::reference);
    return first;
}
struct Sink {
    std::string* output;
    Z size,limit;
    Work& work;
    void raw(SV value) {
        const auto next=add(size,value.size());bound(next,limit);work.debit(value.size());
        if(output)output->append(value);
        size=next;
    }
    void hex(SV value) {
        static constexpr SV digits="0123456789abcdef";
        if(value.empty())return;
        const char* current=value.data();const char* end=current+value.size();
        while(current!=end) {
            const auto byte=static_cast<unsigned char>(*current++);
            work.debit();raw(digits.substr(byte>>4U,1));raw(digits.substr(byte&15U,1));
        }
    }
    void digest(const Digest& value) {
        const auto& bytes=value.bytes();hex(SV{reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    }
    void number(Z value) {
        std::array<char,std::numeric_limits<Z>::digits10+1> digits{};Z count{};
        do {digits[digits.size()-1-count]=static_cast<char>('0'+value%10);value/=10;++count;}while(value);
        raw(SV{digits.data()+digits.size()-count,count});
    }
};
void emit(Sink& out,const State& state,const ns::Owner& child) {
    const bool plugins=detail::uses_plugins(state.workloads);
    out.raw("{\"bindings\":[");bool first=true;
    for(const auto& binding:state.bindings) {
        out.work.debit();if(!first)out.raw(",");first=false;
        out.raw("[\"");out.number(binding.experiment_index);out.raw("\",\"");out.number(binding.workload_index);out.raw("\"]");
    }
    out.raw("],\"normalization_digest\":\"");out.digest(*child.digest());
    out.raw("\",\"normalization_hex\":\"");out.hex(child.canonical());
    out.raw("\",\"resolved_plan_digest\":\"");out.digest(child.plan()->resolved_digest()->digest());
    out.raw("\",\"scenario_digest\":\"");out.digest(child.plan()->scenario_digest()->digest());
    out.raw("\",\"schema\":\"");out.raw(plugins?plugin_profile:profile);out.raw("\",\"workloads\":[");first=true;
    for(const auto& value:state.workloads) {
        out.work.debit();if(!first)out.raw(",");first=false;
        out.raw("[\"");out.hex(value.workload_id);out.raw("\",\"");out.raw(kernel::kernel_id_token(value.semantic));
        out.raw("\",\"");out.raw(kernel::kernel_implementation_id_token(value.implementation));out.raw("\"");
        if(plugins) {
            out.raw(",");
            if(value.module) {
                out.raw("{\"build_info_hex\":\"");out.hex(value.module->build_info);
                out.raw("\",\"family\":\"");out.raw(value.semantic.family()==kernel::KernelFamily::bandwidth?"bandwidth":"latency");
                out.raw("\",\"file_sha256\":\"");out.raw(value.module->file_sha256);
                out.raw("\",\"version_hex\":\"");out.hex(value.module->version);out.raw("\"}");
            } else out.raw("null");
        }
        out.raw("]");
    }
    out.raw("]}");
}
// Named logical frame inventory, independent of allocator bookkeeping or ABI stack maxima.
struct RetainCall {
    ns::Owner* input;Limits limits;Usage measured;std::unique_ptr<State> candidate;
    std::expected<Owner,Error> returned;std::expected<void,Error> checked;std::optional<Error> failure;
    Z C,H,Nchild,W,B,T,M,D,N,R,peak;
};
struct StateConstruction {State* allocated;std::unique_ptr<State>* destination;const Digest* initial_digest;Usage empty_usage;};
struct RowConstruction {std::vector<Workload>* rows;Workload* row;SV key;std::vector<Binding>* bindings;Binding value;};
struct ChildView {
    const ns::Owner* child;const ns::Provenance* provenance;const private_plan_composition::Owner* plan;
    const kernel_dispatch_serialization::Owner* dispatch;const Workloads* workloads;const Bindings* bindings;
    SV canonical;const Digest* child_digest;const scenario::ScenarioDigest* scenario;const scenario::ResolvedScenarioPlanDigest* resolved;
};
struct MapVisit {Workloads::const_iterator current,end;const Workloads::value_type* entry;const scenario::DispatchedWorkload* source;Workload* destination;const kernel::KernelDescriptor* descriptor;SV semantic,implementation;Z ordinal;};
struct BindingVisit {Bindings::const_iterator current,end;const scenario::ExperimentWorkloadBinding* source;Binding* destination;Z ordinal,row;};
struct Search {std::span<const Workload> rows;SV key;Z first,last,middle;const Workload* candidate;bool less,equal;};
struct WorkCells {Z used,limit,amount,next;SV left,right;bool comparison;};
struct SinkCall {Work* work;std::string* output;Z size,limit,amount,next,index;SV value;const Digest* digest;unsigned byte,nibble;std::array<char,std::numeric_limits<Z>::digits10+1> decimal;Z number,digit_count;bool first;};
struct EmitCall {const State* state;const ns::Owner* child;Sink* sink;std::span<const Workload> workloads;std::span<const Binding> bindings;Z row,index;SV semantic,implementation;const Workload* workload;const Binding* binding;};
struct CheckedArithmetic {Z left,right,result,ceiling;std::optional<Z> product;bool accepted;};
struct RegistryCalls {kernel::KernelId semantic;kernel::KernelImplementationId implementation;const kernel::KernelDescriptor* result;SV semantic_token,implementation_token;};
struct MovePublication {Owner* receiver;std::unique_ptr<State>* destination;std::unique_ptr<State> argument;ns::Owner* child_receiver;ns::Owner* child_argument;std::expected<Owner,Error> returned;};
struct LowerUsageCall {const ns::Owner* receiver;ns::Usage returned;};
struct HexTraversal {Sink* receiver;SV source;const char* current;const char* end;const std::array<std::byte,32>* digest_bytes;unsigned char byte;};
struct RetainLocals {Work work;Z prefix,typed,before_emit,hash_work;};
struct TokenScan {SV text;const char* current;const char* end;unsigned char byte;bool valid;};
struct HashCells {
    std::array<std::uint32_t,8> state,working;std::array<std::uint32_t,64> schedule;
    std::array<std::byte,128> final_blocks;std::array<std::byte,32> result,constructor_argument;
    std::array<std::uint32_t,8>* compress_state;const std::byte* block;
    std::uint32_t s0,s1,sum0,sum1,choose,majority,t1,t2;
    Z offset,schedule_index,schedule_offset,round_index,remainder,length_index,word,byte_index,copy_index;
    unsigned final_size;std::uint64_t bit_length;std::span<const std::byte> bytes,tail;SV text;
    Digest returned;Digest* constructor_this;
};
}
namespace detail {
auto checked_add(Z a,Z b) noexcept -> std::optional<Z> {if(b>std::numeric_limits<Z>::max()-a)return {};return a+b;}
auto checked_multiply(Z a,Z b) noexcept -> std::optional<Z> {if(a&&b>std::numeric_limits<Z>::max()/a)return {};return a*b;}
auto fixed_stock() noexcept -> Z {
    return sizeof(RetainCall)+sizeof(StateConstruction)+sizeof(RowConstruction)+sizeof(ChildView)+
        sizeof(MapVisit)+sizeof(BindingVisit)+sizeof(Search)+sizeof(WorkCells)+sizeof(SinkCall)+
        sizeof(EmitCall)+sizeof(CheckedArithmetic)+sizeof(RegistryCalls)+sizeof(MovePublication)+
        sizeof(LowerUsageCall)+sizeof(HexTraversal)+sizeof(RetainLocals)+sizeof(TokenScan)+sizeof(HashCells);
}
auto find_workload(std::span<const Workload> rows,SV key,Z& used,Z limit) -> std::expected<Z,Error> {
    if(used>limit)return std::unexpected{Error::capacity};
    Work work{used,limit};
    try {const auto index=search(rows,key,work);used=work.used;return index;}
    catch(Error error){used=work.used;return std::unexpected{error};}
}
auto mapping_valid(kernel::KernelId id,kernel::KernelImplementationId implementation,SV token) noexcept -> bool {
    return id.family()!=kernel::KernelFamily::invalid&&id.family()==implementation.family()&&
        safe_token(token)&&kernel::kernel_implementation_id_token(implementation)==token;
}
auto uses_plugins(std::span<const Workload> rows) noexcept -> bool {
    return std::ranges::any_of(rows,[](const auto& row) {
        return row.module.has_value() ||
            (row.semantic!=kernel::KernelId::sequential_read_bandwidth_v1&&row.semantic!=kernel::KernelId::dependent_load_latency_v1) ||
            (row.implementation!=kernel::KernelImplementationId::scalar_u64_sequential_v1&&
             row.implementation!=kernel::KernelImplementationId::scalar_u64_random_cycle_v1);
    });
}
auto valid_module(const kernel::ModuleIdentity& module) noexcept -> bool {
    return !module.version.empty()&&module.version.size()<=128U&&
        !module.build_info.empty()&&module.build_info.size()<=4096U&&module.file_sha256.size()==64U&&
        std::ranges::all_of(module.file_sha256,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
}
Owner::Owner(std::unique_ptr<State> state) noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept -> Owner& =default;
Owner::~Owner()=default;
auto Owner::normalization() const noexcept -> const ns::Owner* {return state_&&state_->child?&*state_->child:nullptr;}
auto Owner::workloads() const noexcept -> std::span<const Workload> {return state_?std::span<const Workload>{state_->workloads}:std::span<const Workload>{};}
auto Owner::bindings() const noexcept -> std::span<const Binding> {return state_?std::span<const Binding>{state_->bindings}:std::span<const Binding>{};}
auto Owner::canonical() const noexcept -> SV {return state_?SV{state_->canonical}:SV{};}
auto Owner::digest() const noexcept -> const Digest* {return state_?&state_->digest:nullptr;}
auto Owner::usage() const noexcept -> Usage {return state_?state_->usage:Usage{};}
auto retain(ns::Owner&& input,Limits limits) -> std::expected<Owner,Error> {
    try {
        need(input.origin().has_value(),Error::state);
        need(input.origin()==ns::Origin::session_selected,Error::origin);
        need(input.provenance()&&input.source()&&input.plan()&&input.topology()&&input.digest()&&
             !input.canonical().empty()&&input.plan()->scenario_digest()&&input.plan()->resolved_digest(),Error::source);
        const auto* dispatch=input.plan()->kernel_dispatch();
        need(dispatch&&dispatch->workloads()&&dispatch->experiment_workloads(),Error::source);
        const auto& workloads=*dispatch->workloads();const auto& bindings=*dispatch->experiment_workloads();
        Usage usage{};usage.child_retained_bytes=input.usage().retained_bytes;usage.child_canonical_bytes=input.canonical().size();
        need(usage.child_retained_bytes>=sizeof(ns::Owner),Error::source);
        bound(usage.child_retained_bytes,limits.child_retained_bytes);bound(usage.child_canonical_bytes,limits.child_canonical_bytes);
        usage.workloads=workloads.size();usage.bindings=bindings.size();bound(usage.workloads,limits.workloads);bound(usage.bindings,limits.bindings);
        const auto prefix=add(detail::fixed_stock(),usage.child_retained_bytes);bound(prefix,limits.consumer_simultaneous_bytes);
        Work work{0,limits.association_work};
        for(const auto& [key,value]:workloads) {
            work.debit();need(work.equal(key,value.workload_id),Error::reference);
            usage.workload_text_bytes=add(usage.workload_text_bytes,key.size());
            usage.maximum_workload_text=std::max(usage.maximum_workload_text,key.size());
            const auto* descriptor=kernel::find_kernel_descriptor(value.kernel_id);need(descriptor,Error::registry);
            if(const auto& module=descriptor->module_identity();module) {
                need(detail::valid_module(*module),Error::registry);
                const auto text=add(add(module->version.size(),module->build_info.size()),module->file_sha256.size());
                work.debit(text);usage.workload_text_bytes=add(usage.workload_text_bytes,text);
                usage.maximum_workload_text=std::max({usage.maximum_workload_text,module->version.size(),module->build_info.size(),module->file_sha256.size()});
            }
            bound(usage.workload_text_bytes,limits.workload_text_bytes);bound(usage.maximum_workload_text,limits.maximum_workload_text);
        }
        const auto dynamic=add(add(mul(usage.workloads,sizeof(Workload)),mul(usage.bindings,sizeof(Binding))),usage.workload_text_bytes);
        const auto typed=add(add(add(prefix,sizeof(State)),dynamic),sizeof(Owner));bound(typed,limits.consumer_simultaneous_bytes);
        auto state=std::unique_ptr<State>{new State{{},{},{},{},*input.digest(),{}}};
        state->workloads.reserve(usage.workloads);state->bindings.reserve(usage.bindings);
        for(const auto& [key,value]:workloads) {
            work.debit();need(work.equal(value.kernel_token,kernel::kernel_id_token(value.kernel_id)),Error::reference);
            work.debit();const auto* descriptor=kernel::find_kernel_descriptor(value.kernel_id);need(descriptor!=nullptr,Error::registry);
            need(descriptor->kernel_id()==value.kernel_id,Error::registry);
            const auto semantic=kernel::kernel_id_token(value.kernel_id);const auto implementation=kernel::kernel_implementation_id_token(descriptor->implementation_id());
            work.debit(add(semantic.size(),implementation.size()));need(safe_token(semantic)&&safe_token(implementation),Error::registry);
            work.debit(key.size());state->workloads.emplace_back();auto& row=state->workloads.back();row.workload_id=key;
            row.semantic=value.kernel_id;row.implementation=descriptor->implementation_id();
            row.module=descriptor->module_identity();
        }
        for(const auto& value:bindings) {
            work.debit();const auto index=search(state->workloads,value.workload_id,work);
            state->bindings.push_back({value.experiment_index,index});
        }
        const auto before_emit=work.used;
        {Sink measure{nullptr,0,limits.canonical_bytes,work};emit(measure,*state,input);usage.canonical_bytes=measure.size;}
        bound(usage.canonical_bytes,std::numeric_limits<std::uint64_t>::max()/8U);
        usage.retained_bytes=add(add(add(sizeof(Owner),sizeof(State)),add(dynamic,usage.canonical_bytes)),usage.child_retained_bytes-sizeof(ns::Owner));
        usage.consumer_simultaneous_bytes=add(typed,usage.canonical_bytes);
        bound(usage.retained_bytes,limits.retained_bytes);bound(usage.consumer_simultaneous_bytes,limits.consumer_simultaneous_bytes);
        const auto hash_work=add(add(1,usage.canonical_bytes),mul(64,add(usage.canonical_bytes/64,usage.canonical_bytes%64>=56?2:1)));
        bound(add(work.used,add(work.used-before_emit,hash_work)),work.limit);
        state->canonical.reserve(usage.canonical_bytes);
        {Sink output{&state->canonical,0,usage.canonical_bytes,work};emit(output,*state,input);need(output.size==usage.canonical_bytes,Error::encoding);}
        work.debit(hash_work);state->digest=core::sha256(state->canonical);
        usage.association_work=work.used;state->usage=usage;
        static_assert(std::is_nothrow_move_constructible_v<ns::Owner>);
        static_assert(std::is_nothrow_move_constructible_v<Owner>);
        static_assert(std::is_nothrow_move_constructible_v<std::expected<Owner,Error>>);
        state->child.emplace(std::move(input));return Owner{std::move(state)};
    } catch(Error error){return std::unexpected{error};}
      catch(const std::length_error&){return std::unexpected{Error::capacity};}
      catch(const std::bad_alloc&){throw;}
      catch(...){return std::unexpected{Error::encoding};}
}
} // namespace epycbench::results::implementation_selection
