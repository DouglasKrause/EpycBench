#include "results/private_build_object_association.hpp"
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace epycbench::results::build_object_association {
namespace {
using Z=std::size_t;using SV=std::string_view;using D=core::Sha256Digest;
namespace sel=implementation_selection;namespace b=base_build_serialization;
namespace a=kernel_approval;namespace r=kernel_report;namespace ns=normalization_serialization;
using S=sel::Owner;using B=b::Owner;using A=ApprovalInputs;using P=Owner;using T=detail::State;
using Value=r::Value;
constexpr std::array semantic{kernel::KernelId::sequential_read_bandwidth_v1,kernel::KernelId::dependent_load_latency_v1};
constexpr std::array implementation{kernel::KernelImplementationId::scalar_u64_sequential_v1,kernel::KernelImplementationId::scalar_u64_random_cycle_v1};
constexpr std::array<SV,2> compiler_names{"GCC","Clang"},presets{"release","release-clang"};
constexpr std::array<SV,4> matches{"equal","different","not_available","not_comparable"};
void need(bool value,Error error){if(!value)throw error;}
auto add(Z left,Z right)->Z{const auto result=detail::checked_add(left,right);need(result.has_value(),Error::capacity);return *result;}
auto mul(Z left,Z right)->Z{const auto result=detail::checked_multiply(left,right);need(result.has_value(),Error::capacity);return *result;}
void bound(Z value,Z limit){need(value<=limit,Error::capacity);}
struct Work {
    Z used,limit;
    void debit(Z amount=1){bound(amount,limit-used);used+=amount;}
    auto equal(SV left,SV right)->bool{debit(add(1,add(left.size(),right.size())));return left==right;}
    void test(bool value,Error error){debit();need(value,error);}
};
struct ReportBorrow {
    const r::Owner* owner{};
    const Value *root{},*source{},*compiler{},*qualification{},*object{},*function{};
    SV revision,dirty,family,version,target,preset,object_digest,function_digest;
};
struct CapturedFacts {
    std::array<FamilyFacts,2> rows{};
    std::array<ReportBorrow,4> reports{};
    std::optional<build_manifest::Input> build_input;
    std::optional<CompilerFamily> compiler_family;
};
auto member(const Value& receiver,SV key,Value::Kind kind,Work& work)->const Value& {
    const auto children=receiver.children();
    for(const auto& child:children){work.debit();if(work.equal(child.name(),key)){work.test(child.kind()==kind,Error::association);return child;}}
    throw Error::association;
}
auto alias(SV text,Work& work)->std::optional<CompilerFamily> {
    if(work.equal(text,"GNU")||work.equal(text,"GCC"))return CompilerFamily::gcc;
    if(work.equal(text,"Clang"))return CompilerFamily::clang;
    return {};
}
auto family_lookup(kernel::KernelId id,kernel::KernelImplementationId selected,Work& work)->std::optional<Z> {
    for(Z index=0;index<2;++index){
        work.debit();if(id!=semantic[index])continue;
        work.debit();if(selected==implementation[index])return index;
    }
    return {};
}
void capture(CapturedFacts& facts,const S& selection,const B& base,const A& approvals,Work& work) {
    const auto* normal=selection.normalization();
    work.test(normal&&normal->provenance()&&selection.digest(),Error::state);
    work.test(base.provenance()&&base.digest(),Error::state);
    work.test(normal->origin()==ns::Origin::session_selected&&base.origin()==b::Origin::session_selected,Error::origin);
    work.test(work.equal(normal->provenance()->run,base.provenance()->run)&&normal->provenance()->domain==base.provenance()->domain,Error::provenance);
    work.test(normal->provenance()->sequence!=0&&base.provenance()->sequence!=0,Error::provenance);
    if(normal->provenance()->sequence==base.provenance()->sequence)
        work.test(work.equal(normal->provenance()->time,base.provenance()->time),Error::provenance);
    if(base.manifest()){
        facts.build_input=base.manifest()->input();work.test(facts.build_input.has_value(),Error::state);
        facts.compiler_family=alias(facts.build_input->compiler.id,work);
    }
    for(const auto& row:selection.workloads()){
        work.debit();const auto family=family_lookup(row.semantic,row.implementation,work);
        work.test(family.has_value(),Error::association);facts.rows[*family].declared_workloads=add(facts.rows[*family].declared_workloads,1);
    }
    for(const auto& binding:selection.bindings()){
        work.debit();work.test(binding.workload_index<selection.workloads().size(),Error::association);
        const auto& row=selection.workloads()[binding.workload_index];const auto family=family_lookup(row.semantic,row.implementation,work);
        work.test(family.has_value(),Error::association);facts.rows[*family].bound_experiments=add(facts.rows[*family].bound_experiments,1);
    }
    for(Z family=0;family<2;++family){
        work.debit();facts.rows[family].approval_supplied=approvals[family].has_value();
        for(auto& report:facts.rows[family].reports)report.comparisons.fill(Match::not_available);
        if(!approvals[family])continue;
        const auto* manifest=approvals[family]->manifest();work.test(manifest&&approvals[family]->digest(),Error::state);
        work.test(work.equal(manifest->implementation_id,kernel::kernel_implementation_id_token(implementation[family])),Error::association);
        for(Z slot=0;slot<2;++slot){
            work.debit();auto& borrowed=facts.reports[family*2+slot];borrowed.owner=approvals[family]->report(slot);
            work.test(borrowed.owner!=nullptr,Error::state);const auto view=borrowed.owner->report();
            work.test(view.has_value(),Error::state);work.test(view->family==(family==0?r::Family::bandwidth:r::Family::latency),Error::association);
            borrowed.root=&view->content;
            borrowed.source=&member(*borrowed.root,"source",Value::Kind::object,work);
            borrowed.compiler=&member(*borrowed.root,"compiler",Value::Kind::object,work);
            borrowed.qualification=&member(*borrowed.root,"qualification",Value::Kind::object,work);
            borrowed.object=&member(*borrowed.root,"object",Value::Kind::object,work);
            borrowed.function=&member(*borrowed.root,"function",Value::Kind::object,work);
            borrowed.revision=member(*borrowed.source,"commit",Value::Kind::string,work).text();
            borrowed.dirty=member(*borrowed.source,"dirty",Value::Kind::boolean,work).text();
            borrowed.family=member(*borrowed.compiler,"family",Value::Kind::string,work).text();
            borrowed.version=member(*borrowed.compiler,"version",Value::Kind::string,work).text();
            borrowed.target=member(*borrowed.compiler,"target",Value::Kind::string,work).text();
            borrowed.preset=member(*borrowed.qualification,"preset",Value::Kind::string,work).text();
            borrowed.object_digest=member(*borrowed.object,"sha256",Value::Kind::string,work).text();
            borrowed.function_digest=member(*borrowed.function,"sha256",Value::Kind::string,work).text();
            work.test(work.equal(borrowed.family,compiler_names[slot]),Error::association);
            work.test(work.equal(borrowed.preset,presets[slot]),Error::association);
            work.test(work.equal(borrowed.revision,manifest->source_commit),Error::association);
            work.test(work.equal(borrowed.dirty,"false"),Error::association);
            work.test(work.equal(borrowed.object_digest,manifest->artifacts[slot].object_sha256),Error::association);
            work.test(work.equal(borrowed.function_digest,manifest->artifacts[slot].function_sha256),Error::association);
            if(!facts.build_input)continue;
            auto& result=facts.rows[family].reports[slot].comparisons;const auto& build=*facts.build_input;
            if(facts.compiler_family){work.debit();result[0]=static_cast<Z>(*facts.compiler_family)==slot?Match::equal:Match::different;}
            else result[0]=Match::not_comparable;
            result[1]=work.equal(build.compiler.target,borrowed.target)?Match::equal:Match::different;
            result[2]=work.equal(build.compiler.version,borrowed.version)?Match::equal:Match::different;
            result[3]=Match::not_comparable;
            result[4]=work.equal(build.preset,borrowed.preset)?Match::equal:Match::different;
            work.debit();result[5]=build.git.dirty?Match::different:Match::equal;
            result[6]=work.equal(build.git.revision,borrowed.revision)?Match::equal:Match::different;
        }
    }
}
struct Sink {
    std::string* output;Work* work;Z size,limit;
    void raw(SV value){const auto next=add(size,value.size());bound(next,limit);work->debit(value.size());if(output)output->append(value);size=next;}
    void hex(SV source){
        static constexpr SV digits="0123456789abcdef";if(source.empty())return;
        const char* current=source.data();const char* end=current+source.size();
        while(current!=end){const auto byte=static_cast<unsigned char>(*current++);work->debit();raw(digits.substr(byte>>4U,1));raw(digits.substr(byte&15U,1));}
    }
    void digest(const D& value){const auto& bytes=value.bytes();hex(SV{reinterpret_cast<const char*>(bytes.data()),bytes.size()});}
    void number(Z value){std::array<char,std::numeric_limits<Z>::digits10+1> decimal{};Z digits{};
        do{work->debit();decimal[decimal.size()-1-digits]=static_cast<char>('0'+value%10);value/=10;++digits;}while(value);
        raw(SV{decimal.data()+decimal.size()-digits,digits});}
};
void emit(Sink& out,const CapturedFacts& facts,const S& selection,const B& base,const A& approvals){
    out.raw("{\"base_build_digest\":\"");out.digest(*base.digest());out.raw("\",\"base_build_hex\":\"");out.hex(base.canonical());
    out.raw("\",\"build_manifest_digest\":");if(base.manifest()){out.raw("\"");out.digest(base.manifest()->digest()->value());out.raw("\"");}else out.raw("null");
    out.raw(",\"families\":[");
    for(Z family=0;family<2;++family){out.work->debit();if(family)out.raw(",");out.raw("{\"approval\":");
        if(approvals[family]){
            out.raw("{\"digest\":\"");out.digest(*approvals[family]->digest());out.raw("\",\"hex\":\"");out.hex(approvals[family]->bytes());
            out.raw("\",\"human_hex\":[");for(Z slot=0;slot<2;++slot){out.work->debit();if(slot)out.raw(",");out.raw("\"");out.hex(approvals[family]->human_bytes(slot));out.raw("\"");}
            out.raw("],\"machine_hex\":[");for(Z slot=0;slot<2;++slot){out.work->debit();if(slot)out.raw(",");out.raw("\"");out.hex(approvals[family]->report(slot)->bytes());out.raw("\"");}out.raw("]}");
        }else out.raw("null");
        out.raw(",\"bindings\":\"");out.number(facts.rows[family].bound_experiments);out.raw("\",\"compiler_slots\":[");
        for(Z slot=0;slot<2;++slot){out.work->debit();if(slot)out.raw(",");out.raw("{\"comparison\":[");
            for(Z comparison=0;comparison<7;++comparison){out.work->debit();if(comparison)out.raw(",");out.raw("\"");
                const auto token=detail::match_token(facts.rows[family].reports[slot].comparisons[comparison]);need(token.has_value(),Error::encoding);out.raw(*token);out.raw("\"");}
            out.raw("],\"compiler_family\":\"");out.raw(compiler_names[slot]);out.raw("\",\"function_sha256\":");
            if(approvals[family]){out.raw("\"");out.raw(facts.reports[family*2+slot].function_digest);out.raw("\"");}else out.raw("null");
            out.raw(",\"object_sha256\":");if(approvals[family]){out.raw("\"");out.raw(facts.reports[family*2+slot].object_digest);out.raw("\"");}else out.raw("null");
            out.raw(",\"qualification_preset\":\"");out.raw(presets[slot]);out.raw("\"}");}
        out.raw("],\"declared_workloads\":\"");out.number(facts.rows[family].declared_workloads);
        out.raw("\",\"implementation_id\":\"");out.raw(kernel::kernel_implementation_id_token(implementation[family]));
        out.raw("\",\"semantic_id\":\"");out.raw(kernel::kernel_id_token(semantic[family]));out.raw("\"}");}
    out.raw("],\"implementation_selection_digest\":\"");out.digest(*selection.digest());out.raw("\",\"implementation_selection_hex\":\"");out.hex(selection.canonical());out.raw("\",\"schema\":\"");out.raw(profile);out.raw("\"}");
}
// Named logical cells cover nested getter returns and helper arguments, not ABI padding estimates.
struct RetainCall {S* selection;B* base;A* approvals;Limits limits;Usage measured;std::unique_ptr<T> candidate;std::expected<P,Error> returned;std::optional<Error> failure;Z C_s,C_b,C_a,C,E,H,N,R,peak,hash_work,second_work,first_work;};
struct ChildViews {const ns::Owner* normalization;const ns::Provenance* normalization_provenance;const b::Provenance* base_provenance;const build_manifest::Owner* manifest;std::array<const a::Manifest*,2> approvals;const D* selection_digest;const D* base_digest;const build_manifest::ManifestDigest* manifest_digest;std::span<const sel::Workload> workloads;std::span<const sel::Binding> bindings;};
struct LowerUsage {const S* selection;sel::Usage selection_usage;const B* base;b::Usage base_usage;const a::Owner* approval;a::Usage approval_usage;};
struct GetterReturns {const build_manifest::Owner* manifest;std::optional<build_manifest::Input> build;const r::Owner* report;std::optional<r::ReportView> report_view;const a::Owner* approval;const a::Manifest* approval_manifest;SV raw;std::optional<ns::Origin> normalization_origin;std::optional<b::Origin> base_origin;std::optional<process_subject::BuildState> disposition;};
struct WorkCells {Z used,limit,amount,next;SV left,right;bool comparison;};
struct AssociationVisit {Z family,slot,index,count;const sel::Workload* workload;const sel::Workload* current_workload;const sel::Workload* end_workload;const sel::Binding* binding;const sel::Binding* current_binding;const sel::Binding* end_binding;const a::Artifact* artifact;const Value* member;kernel::KernelId semantic;kernel::KernelImplementationId implementation;SV token;bool equal,available;std::optional<Z> family_index;const std::optional<a::Owner>* current_approval;const std::optional<a::Owner>* end_approval;ReportFacts* current_report;ReportFacts* end_report;};
struct MemberLookup {Work* work;const Value* receiver;const Value* current;const Value* end;const Value* result;SV key,name;Value::Kind required_kind;Z ordinal;bool matched;std::span<const Value> children;};
struct Comparisons {ReportBorrow* report;const build_manifest::Input* build;ReportFacts* result;SV left,right;Match compared;CompilerFamily family;Z position;bool dirty;};
struct SinkCall {Sink* receiver;SV value;Z next;const D* digest;std::array<char,std::numeric_limits<Z>::digits10+1> decimal;Z number,digits,index;unsigned nibble;char character;};
struct HexTraversal {Sink* receiver;SV source;const char* current;const char* end;const std::array<std::byte,32>* digest_bytes;unsigned char byte;};
struct EmitCall {Sink* sink;const CapturedFacts* facts;const S* selection;const B* base;const A* approvals;Z family,slot,comparison;const a::Owner* approval;const a::Manifest* manifest;const a::Artifact* artifact;const ReportBorrow* report;const D* digest;SV token,raw;bool first,present;std::optional<SV> match;};
struct CheckedArithmetic {Z left,right,result,ceiling;std::optional<Z> sum,product;bool accepted;};
struct GuardAndError {bool predicate;Error error,caught;std::unexpected<Error> returned;const std::length_error* exception;};
struct CaptureCall {CapturedFacts* facts;const S* selection;const B* base;const A* approvals;Work* work;};
struct AliasCall {SV text;Work* work;std::optional<CompilerFamily> returned;};
struct FamilyLookup {kernel::KernelId semantic;kernel::KernelImplementationId implementation;Work* work;Z index;std::optional<Z> returned;};
struct TokenCalls {Match match;CompilerFamily family;kernel::KernelId semantic;kernel::KernelImplementationId implementation;Z index;SV returned;std::optional<SV> match_return;};
struct DigestGetter {const S* selection;const B* base;const a::Owner* approval;const build_manifest::Owner* build;const D* returned;const build_manifest::ManifestDigest* manifest;const std::array<std::byte,32>* bytes;};
struct StringLimit {std::string value;};
struct StateConstruction {T* allocated;std::unique_ptr<T>* destination;const CapturedFacts* source;Usage empty_usage;D initial_digest;};
struct MovePublication {P* receiver;std::unique_ptr<T>* destination;std::unique_ptr<T> argument;S* selection_receiver;S* selection_argument;B* base_receiver;B* base_argument;a::Owner* approval_receiver;a::Owner* approval_argument;Z slot;std::expected<P,Error> returned;};
struct HashCells {
    std::array<std::uint32_t,8> state,working;std::array<std::uint32_t,64> schedule;
    std::array<std::byte,128> final_blocks;std::array<std::byte,32> result,constructor_argument;
    std::array<std::uint32_t,8>* compress_state;const std::byte* block;
    std::uint32_t s0,s1,sum0,sum1,choose,majority,t1,t2;
    Z offset,schedule_index,schedule_offset,round_index,remainder,length_index,word,byte_index,copy_index;
    unsigned final_size;std::uint64_t bit_length;std::span<const std::byte> bytes,tail;SV text;
    D returned;D* constructor_this;
};
}
namespace detail {
auto checked_add(Z left,Z right) noexcept->std::optional<Z>{if(right>std::numeric_limits<Z>::max()-left)return {};return left+right;}
auto checked_multiply(Z left,Z right) noexcept->std::optional<Z>{if(left&&right>std::numeric_limits<Z>::max()/left)return {};return left*right;}
auto compiler_family(SV text) noexcept->std::optional<CompilerFamily>{if(text=="GNU"||text=="GCC")return CompilerFamily::gcc;if(text=="Clang")return CompilerFamily::clang;return {};}
auto family_index(kernel::KernelId id,kernel::KernelImplementationId selected) noexcept->std::optional<Z>{for(Z i=0;i<2;++i)if(id==semantic[i]&&selected==implementation[i])return i;return {};}
auto match_token(Match value) noexcept->std::optional<SV>{const auto index=static_cast<Z>(value);if(index>=matches.size())return {};return matches[index];}
auto fixed_stock() noexcept->Z{
    return sizeof(RetainCall)+sizeof(CapturedFacts)+sizeof(ChildViews)+sizeof(LowerUsage)+sizeof(GetterReturns)+sizeof(WorkCells)+sizeof(AssociationVisit)+sizeof(MemberLookup)+sizeof(Comparisons)+sizeof(Sink)+sizeof(SinkCall)+sizeof(HexTraversal)+sizeof(EmitCall)+sizeof(CheckedArithmetic)+sizeof(GuardAndError)+sizeof(CaptureCall)+sizeof(AliasCall)+sizeof(FamilyLookup)+sizeof(TokenCalls)+sizeof(DigestGetter)+sizeof(StringLimit)+sizeof(StateConstruction)+sizeof(MovePublication)+sizeof(HashCells);
}
}
Owner::Owner(std::unique_ptr<T> state) noexcept:state_(std::move(state)){}
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept->Owner& = default;
Owner::~Owner()=default;
auto Owner::selection() const noexcept->const S*{return state_&&state_->selection?&*state_->selection:nullptr;}
auto Owner::base() const noexcept->const B*{return state_&&state_->base?&*state_->base:nullptr;}
auto Owner::approval(Z slot) const noexcept->const a::Owner*{return state_&&slot<2&&state_->approvals[slot]?&*state_->approvals[slot]:nullptr;}
auto Owner::families() const noexcept->std::span<const FamilyFacts>{return state_?std::span<const FamilyFacts>{state_->families}:std::span<const FamilyFacts>{};}
auto Owner::canonical() const noexcept->SV{return state_?SV{state_->canonical}:SV{};}
auto Owner::digest() const noexcept->const D*{return state_?&state_->digest:nullptr;}
auto Owner::usage() const noexcept->Usage{return state_?state_->usage:Usage{};}
auto retain(S&& selection,B&& base,A&& approvals,Limits limits)->std::expected<Owner,Error>{
    try{
        Work work{0,limits.association_work};
        work.test(selection.normalization()&&selection.digest()&&base.provenance()&&base.digest(),Error::state);
        Usage measured;measured.workloads=selection.workloads().size();measured.bindings=selection.bindings().size();
        bound(measured.workloads,limits.workloads);bound(measured.bindings,limits.bindings);
        const Z C_s=selection.usage().retained_bytes,C_b=base.usage().retained_bytes;
        need(C_s>=sizeof(S)&&C_b>=sizeof(B),Error::state);
        Z C_a=0;Z E=add(selection.canonical().size(),base.canonical().size());
        for(const auto& approval:approvals){work.debit();if(approval){
            work.test(approval->manifest()&&approval->digest(),Error::state);measured.approvals=add(measured.approvals,1);
            C_a=add(C_a,approval->usage().retained_bytes);E=add(E,approval->bytes().size());
            for(Z slot=0;slot<2;++slot){work.debit();work.test(approval->report(slot)!=nullptr,Error::state);E=add(E,add(approval->report(slot)->bytes().size(),approval->human_bytes(slot).size()));}
        }}
        const Z C=add(add(C_s,C_b),add(sizeof(A),C_a));
        measured.child_retained_bytes=C;measured.child_evidence_bytes=E;
        bound(measured.approvals,limits.approvals);bound(C,limits.child_retained_bytes);bound(E,limits.child_evidence_bytes);
        bound(add(detail::fixed_stock(),C),limits.consumer_simultaneous_bytes);
        CapturedFacts facts;capture(facts,selection,base,approvals,work);
        Z N=0;const Z first_work=work.used;
        {Sink count{nullptr,&work,0,limits.canonical_bytes};emit(count,facts,selection,base,approvals);N=count.size;}
        const Z second_work=work.used-first_work;
        const Z R=add(add(add(add(sizeof(P),sizeof(T)),add(C_s-sizeof(S),C_b-sizeof(B))),C_a),N);
        const Z peak=add(add(add(add(detail::fixed_stock(),C),sizeof(T)),sizeof(P)),N);
        const StringLimit limit_probe;bound(N,limit_probe.value.max_size());bound(N,std::numeric_limits<std::uint64_t>::max()/8U);
        const Z hash_work=add(N,mul(64,add(N/64,N%64<56?1:2)));
        bound(add(work.used,add(second_work,hash_work)),limits.association_work);
        bound(R,limits.retained_bytes);bound(peak,limits.consumer_simultaneous_bytes);
        std::unique_ptr<T> candidate{new T{{},{},{},facts.rows,{},*selection.digest(),{}}};
        candidate->canonical.reserve(N);
        {Sink output{&candidate->canonical,&work,0,N};emit(output,facts,selection,base,approvals);need(output.size==N,Error::encoding);}
        work.debit(hash_work);candidate->digest=core::sha256(candidate->canonical);
        measured.canonical_bytes=N;measured.association_work=work.used;measured.retained_bytes=R;measured.consumer_simultaneous_bytes=peak;candidate->usage=measured;
        static_assert(std::is_nothrow_move_constructible_v<S>&&std::is_nothrow_move_constructible_v<B>&&std::is_nothrow_move_constructible_v<a::Owner>);
        candidate->selection.emplace(std::move(selection));candidate->base.emplace(std::move(base));
        for(Z slot=0;slot<2;++slot)if(approvals[slot]){candidate->approvals[slot].emplace(std::move(*approvals[slot]));approvals[slot].reset();}
        return Owner{std::move(candidate)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
} // namespace epycbench::results::build_object_association
