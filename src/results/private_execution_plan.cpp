#include "results/private_execution_plan.hpp"
#include <limits>
#include <stdexcept>

namespace epycbench::results::execution_plan {
namespace {
using Z=std::size_t;using U=std::uint64_t;using SV=std::string_view;using Json=nlohmann::ordered_json;
using D=core::Sha256Digest;using H=build_object_association::Owner;using T=detail::State;using P=Owner;
using Index=detail::NameIndex;
namespace sel=implementation_selection;namespace ns=normalization_serialization;namespace plan=private_plan_composition;
namespace da=document_ancestry_serialization;namespace ce=comparison_experiments_serialization;namespace me=matrix_experiments_serialization;
namespace kd=kernel_dispatch_serialization;namespace b=base_build_serialization;namespace a=kernel_approval;namespace sc=scenario;
void need(bool value,Error error){if(!value)throw error;}
auto add(Z left,Z right)->Z {auto result=detail::checked_add(left,right);need(result.has_value(),Error::capacity);return *result;}
auto mul(Z left,Z right)->Z {auto result=detail::checked_multiply(left,right);need(result.has_value(),Error::capacity);return *result;}
void bound(Z value,Z limit){need(value<=limit,Error::capacity);}
struct Work {
    Z used,limit;
    void debit(Z amount=1){bound(amount,limit-used);used+=amount;}
    void test(bool value,Error error){debit();need(value,error);}
    auto equal(SV first,SV second)->bool{debit(add(1,add(first.size(),second.size())));return first==second;}
    auto less(SV first,SV second)->bool{debit(add(1,add(first.size(),second.size())));return first<second;}
};
struct Roots {
    const sel::Owner* selection{};const ns::Owner* normalization{};const plan::Owner* plan{};
    const da::Owner* ancestry{};const ce::Owner* comparison{};const me::Owner* matrix{};const kd::Owner* kernel{};
    const b::Owner* base{};const build_manifest::Owner* manifest{};
    const Json *effective{},*measurement{},*monitoring{},*random{},*repetition{};
    std::span<const sel::Workload> workloads;std::span<const sel::Binding> bindings;
    const std::vector<sc::ResolvedComparisonExperiment>* comparisons{};
    const std::vector<sc::ExpandedMatrixExperiment>* matrices{};
};
auto member(const Json& object,SV key,Work& work,bool required=true)->const Json* {
    work.test(object.is_object(),Error::policy);
    for(auto current=object.begin(),end=object.end();current!=end;++current){work.debit();if(work.equal(current.key(),key))return &*current;}
    work.test(!required,Error::policy);return nullptr;
}
auto number(const Json& value,Work& work)->U{work.test(value.is_number_unsigned(),Error::policy);return value.get<U>();}
auto boolean(const Json& value,Work& work)->bool{work.test(value.is_boolean(),Error::policy);return value.get<bool>();}
auto family(const sel::Workload& row,Work& work)->Z{
    for(Z index=0;index<2;++index){work.debit();
        work.debit();if(row.semantic.family()!=(index==0?kernel::KernelFamily::bandwidth:kernel::KernelFamily::latency))continue;
        work.test(row.implementation.family()==row.semantic.family(),Error::object_evidence);return index;
    }throw Error::object_evidence;
}
void roots(Roots& out,const H& input,Work& work){
    work.test(input.digest()&&!input.canonical().empty(),Error::state);
    out.selection=input.selection();out.base=input.base();work.test(out.selection&&out.base,Error::state);
    out.normalization=out.selection->normalization();work.test(out.normalization,Error::state);
    work.test(out.normalization->origin()==ns::Origin::session_selected&&out.base->origin()==b::Origin::session_selected,Error::origin);
    out.plan=out.normalization->plan();work.test(out.plan,Error::state);
    out.ancestry=out.plan->document_ancestry();out.comparison=out.plan->comparison_experiments();out.matrix=out.plan->matrix_experiments();out.kernel=out.plan->kernel_dispatch();
    work.test(out.ancestry&&out.comparison&&out.matrix&&out.kernel,Error::state);
    out.effective=out.ancestry->default_expanded();out.comparisons=out.comparison->experiments();out.matrices=out.matrix->experiments();
    work.test(out.effective&&out.comparisons&&out.matrices&&out.plan->scenario_digest()&&out.plan->resolved_digest(),Error::state);
    out.manifest=out.base->manifest();work.test(out.base->disposition()==process_subject::BuildState::available&&out.manifest&&out.manifest->digest(),Error::build);
    out.workloads=out.selection->workloads();out.bindings=out.selection->bindings();
    out.measurement=member(*out.effective,"measurement",work);out.random=member(*out.effective,"random",work);
    out.monitoring=member(*out.effective,"monitoring",work,false);out.repetition=member(*out.measurement,"repetition",work);
    work.test(out.measurement->is_object()&&(!out.monitoring||out.monitoring->is_object()),Error::policy);
}
void prepare_counts(Counts& counts,const Roots& view,Work& work){
    counts.root_seed=number(*member(*view.random,"seed",work),work);
    counts.minimum_blocks=number(*member(*view.repetition,"minimum_blocks",work),work);
    counts.maximum_blocks=number(*member(*view.repetition,"maximum_blocks",work),work);
    counts.minimum_valid_per_variant=number(*member(*view.repetition,"minimum_valid_observations_per_variant",work),work);
    const auto ceiling=detail::measured_ceiling(counts.maximum_blocks);
    work.test(counts.minimum_blocks<=counts.maximum_blocks&&ceiling.has_value(),Error::policy);
    counts.maximum_observations=*ceiling;counts.maximum_per_variant=counts.maximum_blocks*2U;
    counts.stopping_enabled=boolean(*member(*member(*view.measurement,"stopping",work),"enabled",work),work);
    counts.monitoring_present=view.monitoring!=nullptr;
}
void supplied_evidence(std::array<bool,2>& used,const Roots& view,const H& input,Work& work){
    for(Z index=0;index<view.bindings.size();++index){work.debit();work.test(view.bindings[index].workload_index<view.workloads.size(),Error::reference);used[family(view.workloads[view.bindings[index].workload_index],work)]=true;}
    for(Z index=0;index<view.workloads.size();++index){work.debit();(void)family(view.workloads[index],work);}
    for(Z index=0;index<2;++index){work.debit();if(!used[index])continue;
        const auto* approval=input.approval(index);if(!approval)continue;
        work.test(approval->manifest()&&approval->digest(),Error::object_evidence);
        for(Z slot=0;slot<2;++slot){work.debit();work.test(approval->report(slot)&&!approval->human_bytes(slot).empty(),Error::object_evidence);}
    }
}
auto index_less(const Index& first,const Index& second,Work& work)->bool{
    work.debit();if(first.experiment!=second.experiment)return first.experiment<second.experiment;
    return work.less(first.name,second.name);
}
void sift(std::span<Index> index,Z root,Z size,Work& work){
    while(root<size/2){work.debit();const Z left=root*2+1;Z chosen=left;const Z right=left+1;
        if(right<size&&index_less(index[left],index[right],work))chosen=right;
        if(!index_less(index[root],index[chosen],work))return;
        work.debit();const auto swapped=index[root];index[root]=index[chosen];index[chosen]=swapped;root=chosen;
    }
}
void sort_index(std::span<Index> index,Work& work){
    for(Z start=index.size()/2;start>0;--start){work.debit();sift(index,start-1,index.size(),work);}
    for(Z end=index.size();end>1;--end){work.debit();const auto swapped=index[0];index[0]=index[end-1];index[end-1]=swapped;sift(index,0,end-1,work);}
    for(Z at=1;at<index.size();++at){work.debit();work.test(index[at-1].experiment!=index[at].experiment||!work.equal(index[at-1].name,index[at].name),Error::reference);}
}
auto lookup(std::span<const Index> index,Z experiment,SV key,Work& work)->Z{
    Z lo=0,hi=index.size();while(lo<hi){work.debit();const auto mid=lo+(hi-lo)/2;
        work.debit();if(index[mid].experiment<experiment||(index[mid].experiment==experiment&&work.less(index[mid].name,key)))lo=mid+1;else hi=mid;
    }
    work.test(lo<index.size(),Error::reference);work.test(index[lo].experiment==experiment&&work.equal(index[lo].name,key),Error::reference);return index[lo].variant;
}
struct Placement {
    const sc::ResolvedMemoryPlacement* uniform{};const sc::ResolvedPerWorkerMemoryPlacement* per_worker{};
    std::span<const sc::ResolvedMemoryNode> nodes;
};
void placement(Placement& out,const sc::ComparisonMemoryPlacement& value,Z worker,Work& work){
    out.uniform=std::get_if<sc::ResolvedMemoryPlacement>(&value);out.per_worker=std::get_if<sc::ResolvedPerWorkerMemoryPlacement>(&value);
    if(out.uniform){out.nodes=out.uniform->nodes;return;}
    work.test(out.per_worker&&worker<out.per_worker->worker_nodes.size(),Error::reference);out.nodes={&out.per_worker->worker_nodes[worker].node,1};
}
auto same_invariants(const Placement& first,const Placement& second,Work& work)->bool{
    work.debit();return (first.uniform?first.uniform->page_mode:first.per_worker->page_mode)==(second.uniform?second.uniform->page_mode:second.per_worker->page_mode)&&
        (first.uniform?first.uniform->page_size:first.per_worker->page_size)==(second.uniform?second.uniform->page_size:second.per_worker->page_size)&&
        (first.uniform?first.uniform->placement_threshold:first.per_worker->placement_threshold)==(second.uniform?second.uniform->placement_threshold:second.per_worker->placement_threshold)&&
        (first.uniform?first.uniform->prefault:first.per_worker->prefault)==(second.uniform?second.uniform->prefault:second.per_worker->prefault)&&
        (first.uniform?first.uniform->verify_before:first.per_worker->verify_before)==(second.uniform?second.uniform->verify_before:second.per_worker->verify_before)&&
        (first.uniform?first.uniform->verify_after:first.per_worker->verify_after)==(second.uniform?second.uniform->verify_after:second.per_worker->verify_after);
}
auto different_nodes(const Placement& first,const Placement& second,Work& work)->bool{
    work.debit();if((first.uniform?first.uniform->policy:first.per_worker->policy)!=(second.uniform?second.uniform->policy:second.per_worker->policy)||first.nodes.size()!=second.nodes.size())return true;
    for(Z node=0;node<first.nodes.size();++node){work.debit();if(first.nodes[node].linux_node!=second.nodes[node].linux_node)return true;}return false;
}
void validate_pair(const Roots& roots,const Pair& pair,Work& work){
    bool distinct=false;
    if(pair.kind==PairKind::comparison){
        const auto& experiment=(*roots.comparisons)[pair.child_experiment_index];
        for(Z worker=0;worker<experiment.workers.size();++worker){work.debit();Placement first,second;
            placement(first,experiment.variants[pair.baseline_index].memory_placement,worker,work);
            placement(second,experiment.variants[pair.candidate_index].memory_placement,worker,work);
            work.test(same_invariants(first,second,work),Error::incompatible_placement);
            distinct=different_nodes(first,second,work)||distinct;
        }
    }else{
        const auto& experiment=(*roots.matrices)[pair.child_experiment_index];
        const auto& first_cell=experiment.cells[pair.baseline_index];const auto& second_cell=experiment.cells[pair.candidate_index];
        work.test(first_cell.worker_row_index==second_cell.worker_row_index,Error::reference);
        Placement first{&first_cell.memory_placement,nullptr,first_cell.memory_placement.nodes};
        Placement second{&second_cell.memory_placement,nullptr,second_cell.memory_placement.nodes};
        work.test(same_invariants(first,second,work),Error::incompatible_placement);distinct=different_nodes(first,second,work);
    }
    work.test(distinct,Error::incompatible_placement);
}
void count_shape(Usage& usage,const Roots& view,Work& work){
    usage.experiments=view.bindings.size();
    for(Z at=0;at<view.comparisons->size();++at){work.debit();usage.variants=add(usage.variants,(*view.comparisons)[at].variants.size());usage.pairs=add(usage.pairs,(*view.comparisons)[at].candidate_variant_ids.size());}
    for(Z at=0;at<view.matrices->size();++at){work.debit();usage.pairs=add(usage.pairs,(*view.matrices)[at].comparison_edges.size());}
}
void pairs(T& state,const Roots& roots,std::span<const Index> index,Work& work){
    Z ordinary=0,matrix=0;
    for(Z experiment=0;experiment<roots.bindings.size();++experiment){work.debit();const auto& binding=roots.bindings[experiment];
        work.test(binding.experiment_index==experiment,Error::reference);
        const bool comparison=ordinary<roots.comparisons->size()&&(*roots.comparisons)[ordinary].experiment_index==experiment;
        const bool expanded=matrix<roots.matrices->size()&&(*roots.matrices)[matrix].experiment_index==experiment;
        work.test(comparison!=expanded,Error::reference);
        const auto& configuration=comparison?(*roots.comparisons)[ordinary].comparison:(*roots.matrices)[matrix].comparison;
        work.test(detail::order_token(configuration.order).has_value(),Error::policy);
        Pair value{0,experiment,comparison?ordinary:matrix,binding.workload_index,0,0,comparison?PairKind::comparison:PairKind::matrix,configuration.order,configuration.order_seed.value_or(state.counts.root_seed)};
        if(comparison){const auto& source=(*roots.comparisons)[ordinary];value.baseline_index=lookup(index,experiment,source.baseline_variant_id,work);
            for(Z candidate=0;candidate<source.candidate_variant_ids.size();++candidate){work.debit();value.candidate_index=lookup(index,experiment,source.candidate_variant_ids[candidate],work);
                work.test(value.baseline_index!=value.candidate_index,Error::reference);value.ordinal=state.pairs.size();validate_pair(roots,value,work);work.debit();state.pairs.push_back(value);}
            ++ordinary;
        }else{const auto& source=(*roots.matrices)[matrix];
            for(Z edge=0;edge<source.comparison_edges.size();++edge){work.debit();value.baseline_index=source.comparison_edges[edge].baseline_cell_index;value.candidate_index=source.comparison_edges[edge].candidate_cell_index;
                work.test(value.baseline_index<source.cells.size()&&value.candidate_index<source.cells.size()&&value.baseline_index!=value.candidate_index,Error::reference);
                value.ordinal=state.pairs.size();validate_pair(roots,value,work);work.debit();state.pairs.push_back(value);}
            ++matrix;
        }
    }
    work.test(ordinary==roots.comparisons->size()&&matrix==roots.matrices->size(),Error::reference);
}
struct Sink {
    std::string* output;Z size,limit;Work& work;
    void raw(SV text){work.debit(add(1,text.size()));size=add(size,text.size());bound(size,limit);if(output)output->append(text);}
    void hex_count(Z bytes){work.debit(mul(bytes,4));size=add(size,mul(bytes,2));bound(size,limit);}
    void hex(SV text){
        if(!output){hex_count(text.size());return;}if(text.empty())return;
        constexpr SV digits="0123456789abcdef";
        for(const char* current=text.data(),*end=current+text.size();current!=end;++current){work.debit();const auto byte=static_cast<unsigned char>(*current);const std::array<char,2> pair{digits[byte>>4U],digits[byte&15U]};raw({pair.data(),pair.size()});}
    }
    void quoted(SV text){raw("\"");hex(text);raw("\"");}
    void number(U value){std::array<char,20> buffer{};Z begin=buffer.size();do{work.debit();buffer[--begin]=static_cast<char>('0'+value%10);value/=10;}while(value);raw("\"");raw({buffer.data()+begin,buffer.size()-begin});raw("\"");}
    void ceiling(MeasuredCeiling value){work.test(detail::valid_ceiling(value),Error::encoding);if(value.two_to_64){work.debit(20);raw("\"18446744073709551616\"");}else number(value.low);}
    void digest(const D& value){const auto& bytes=value.bytes();hex({reinterpret_cast<const char*>(bytes.data()),bytes.size()});}
    void flag(bool value){raw(value?"true":"false");}
};
void endpoint(Sink& out,const Roots& view,const Pair& pair,bool baseline){
    const auto at=baseline?pair.baseline_index:pair.candidate_index;out.raw("[");
    if(pair.kind==PairKind::comparison)out.quoted((*view.comparisons)[pair.child_experiment_index].variants[at].variant_id);
    else{const auto& cell=(*view.matrices)[pair.child_experiment_index].cells[at];out.quoted(cell.id.worker_node.value());out.raw(",");out.quoted(cell.id.memory_node.value());}
    out.raw("]");
}
void semantic(Sink& out,const T& state,const Roots& view,const H& child,const std::array<bool,2>& used){
    const bool plugins=sel::detail::uses_plugins(view.workloads);
    out.raw("{\"build_manifest_digest\":\"");out.digest(view.manifest->digest()->value());out.raw("\",\"implementation_evidence\":[");bool first=true;
    for(Z family_index=0;family_index<2;++family_index){out.work.debit();if(!used[family_index]||!child.approval(family_index))continue;if(!first)out.raw(",");first=false;
        const auto* manifest=child.approval(family_index)->manifest();
        out.raw("{\"framework_version\":");out.quoted(manifest->framework_version);out.raw(",\"implementation_id\":\"");
        out.raw(family_index==0?"scalar-u64-sequential/v1":"scalar-u64-random-cycle/v1");out.raw("\",\"policy_version\":");out.quoted(manifest->policy_version);
        out.raw(",\"report_schema\":");out.quoted(manifest->report_schema);out.raw(",\"semantic_id\":\"");out.raw(family_index==0?"memory.bandwidth.read/v1":"memory.latency.dependent-load/v1");out.raw("\",\"slots\":[");
        for(Z slot=0;slot<2;++slot){out.work.debit();if(slot)out.raw(",");const auto& artifact=manifest->artifacts[slot];
            out.raw("{\"compiler_family\":\"");out.raw(slot==0?"GCC":"Clang");out.raw("\",\"function_sha256\":\"");out.raw(artifact.function_sha256);
            out.raw("\",\"object_sha256\":\"");out.raw(artifact.object_sha256);out.raw("\",\"qualification_preset\":\"");out.raw(slot==0?"release":"release-clang");out.raw("\"}");}
        out.raw("],\"source_commit\":");out.quoted(manifest->source_commit);out.raw("}");
    }
    out.raw("],\"implementations\":[");
    for(Z row=0;row<view.workloads.size();++row){out.work.debit();if(row)out.raw(",");const auto& workload=view.workloads[row];out.raw("[");out.quoted(workload.workload_id);out.raw(",\"");out.raw(kernel::kernel_id_token(workload.semantic));out.raw("\",\"");out.raw(kernel::kernel_implementation_id_token(workload.implementation));out.raw("\"]");}
    if(plugins) {
        out.raw("],\"modules\":[");
        for(Z row=0;row<view.workloads.size();++row) {
            out.work.debit();if(row)out.raw(",");const auto& workload=view.workloads[row];
            if(!workload.module){out.raw("null");continue;}
            out.raw("{\"build_info_hex\":");out.quoted(workload.module->build_info);
            out.raw(",\"family\":\"");out.raw(workload.semantic.family()==kernel::KernelFamily::bandwidth?"bandwidth":"latency");
            out.raw("\",\"file_sha256\":\"");out.raw(workload.module->file_sha256);
            out.raw("\",\"implementation_id\":\"");out.raw(kernel::kernel_implementation_id_token(workload.implementation));
            out.raw("\",\"kernel_id\":\"");out.raw(kernel::kernel_id_token(workload.semantic));
            out.raw("\",\"version_hex\":");out.quoted(workload.module->version);out.raw(",\"workload_hex\":");out.quoted(workload.workload_id);out.raw("}");
        }
    }
    out.raw("],\"monitoring_presence\":\"");out.raw(state.counts.monitoring_present?"supplied":"not_supplied");out.raw("\",\"pairs\":[");
    for(Z row=0;row<state.pairs.size();++row){out.work.debit();if(row)out.raw(",");const auto& pair=state.pairs[row];
        out.raw("{\"baseline\":");endpoint(out,view,pair,true);out.raw(",\"candidate\":");endpoint(out,view,pair,false);out.raw(",\"experiment\":");
        out.quoted(pair.kind==PairKind::comparison?(*view.comparisons)[pair.child_experiment_index].experiment_id:(*view.matrices)[pair.child_experiment_index].experiment_id);
        out.raw(",\"experiment_index\":");out.number(pair.experiment_index);out.raw(",\"kind\":\"");out.raw(pair.kind==PairKind::comparison?"comparison":"matrix");
        out.raw("\",\"order\":\"");out.raw(*detail::order_token(pair.order));out.raw("\",\"order_seed\":");out.number(pair.order_seed);out.raw(",\"ordinal\":");out.number(pair.ordinal);
        out.raw(",\"workload_index\":");out.number(pair.workload_index);out.raw("}");
    }
    out.raw("],\"policy\":{\"build_evidence\":\"");
    out.raw(plugins?"build-manifest-and-recorded-modules":view.bindings.empty()||child.approval(0)||child.approval(1)?"supplied-manifest-and-bound-family-objects":"build-manifest");
    out.raw("\",\"current_qualification\":\"not_asserted\",\"production_admission\":\"deferred\",\"schedule\":\"measurement-four-slot-v1\"},\"repetition\":{\"actual_completed\":\"unknown\",\"maximum_blocks\":");
    out.number(state.counts.maximum_blocks);out.raw(",\"maximum_measured_observations\":");out.ceiling(state.counts.maximum_observations);out.raw(",\"maximum_per_variant\":");out.number(state.counts.maximum_per_variant);
    out.raw(",\"minimum_blocks\":");out.number(state.counts.minimum_blocks);out.raw(",\"minimum_valid_per_variant\":");out.number(state.counts.minimum_valid_per_variant);out.raw(",\"stopping_enabled\":");out.flag(state.counts.stopping_enabled);
    out.raw("},\"resolved_plan_digest\":\"");out.digest(view.plan->resolved_digest()->digest());out.raw("\",\"scenario_digest\":\"");out.digest(view.plan->scenario_digest()->digest());
    out.raw("\",\"schema\":\"");out.raw(plugins?plugin_profile:profile);out.raw("\",\"seed_intent\":{\"bootstrap\":{\"components\":[],\"derivation\":\"deferred-to-production-policy\",\"domain\":\"bootstrap\",\"profile\":\"epycbench.seed-derivation.v1\"},\"root_seed\":");out.number(state.counts.root_seed);out.raw("}}");
}
void artifact(Sink& out,const H& input,SV semantic_bytes,Z semantic_size,const D& digest){
    out.raw("{\"evidence_digest\":\"");out.digest(*input.digest());out.raw("\",\"evidence_hex\":\"");out.hex(input.canonical());out.raw("\",\"execution_plan_digest\":\"");out.digest(digest);
    out.raw("\",\"schema\":\"");out.raw(artifact_profile);out.raw("\",\"semantic_hex\":\"");if(out.output)out.hex(semantic_bytes);else out.hex_count(semantic_size);out.raw("\"}");
}
auto hash_work(Z bytes)->Z {need(bytes<=UINT64_MAX/8,Error::capacity);(void)add(bytes,72);return add(bytes,mul(64,add(bytes/64,bytes%64>=56?2U:1U)));}
void final_bounds(const Usage& usage,const Limits& limits){
    bound(usage.experiments,limits.experiments);bound(usage.variants,limits.variants);bound(usage.pairs,limits.pairs);
    bound(usage.child_retained_bytes,limits.child_retained_bytes);bound(usage.child_canonical_bytes,limits.child_canonical_bytes);
    bound(usage.index_bytes,limits.index_bytes);bound(usage.pair_bytes,limits.pair_bytes);bound(usage.semantic_bytes,limits.semantic_bytes);bound(usage.canonical_bytes,limits.canonical_bytes);
    bound(usage.planning_work,limits.planning_work);bound(usage.retained_bytes,limits.retained_bytes);bound(usage.consumer_simultaneous_bytes,limits.consumer_simultaneous_bytes);
}
// Named source-cell stocks; independent test mirrors must not use these definitions.
struct ProduceCall {H* input;Limits limits;Usage usage;Counts counts;std::unique_ptr<T> candidate;std::vector<Index> index;Z C,B,E,V,K,I,D,S,A,R,peak,work_after_pairs,semantic_work,remaining_work;Error error;};
struct LowerUsage {const H* owner;build_object_association::Usage value;};
struct GetterReturns {const ns::Owner* normalization_receiver;const b::Owner* base_receiver;std::optional<ns::Origin> normalization_origin;std::optional<b::Origin> base_origin;std::optional<process_subject::BuildState> build_state;const build_manifest::Owner* manifest;const build_manifest::ManifestDigest* manifest_digest;const D* digest;const std::array<std::byte,32>* digest_bytes;const ExecutionPlanDigest* execution_digest_receiver;SV raw;};
struct EvidenceScan {std::array<bool,2> used;Z family,slot,workload,binding;const a::Owner* approval;const a::Manifest* manifest;const a::Artifact* artifact;std::optional<Z> family_result;SV token;std::array<SV,4> metadata;};
struct CountScan {Z experiment,comparison,matrix,variant,candidate,edge;const sc::ResolvedComparisonExperiment* ordinary;const sc::ExpandedMatrixExperiment* expanded;const sel::Binding* binding;std::optional<Z> checked;};
struct JsonLookup {const Json *object,*value;Json::const_iterator current,end;SV key,name;U unsigned_value;bool boolean_value;};
struct HeapSort {std::span<Index> index;Z size,start,end,root,left,right,chosen;Index swapped;const Index *first,*second;SV left_name,right_name;bool less;};
struct NameLookup {std::span<const Index> index;Z experiment,lo,hi,mid,result;SV key;const Index* item;std::optional<Z> found;};
struct PairWalk {Z experiment,comparison,matrix,candidate,edge,ordinal,baseline,second,workload;const sc::ResolvedComparisonExperiment* ordinary;const sc::ExpandedMatrixExperiment* expanded;const sc::MatrixComparisonEdge* edge_value;const sc::MatrixComparisonConfiguration* config;Pair value;std::optional<Z> baseline_lookup,candidate_lookup;std::optional<SV> order_token;};
struct PlacementView {const sc::ComparisonMemoryPlacement* input;const sc::ResolvedMemoryPlacement* uniform;const sc::ResolvedPerWorkerMemoryPlacement* per_worker;const sc::ResolvedMatrixCell* cell;const sc::ResolvedMemoryNode* node;std::span<const sc::ResolvedMemoryNode> nodes;Z worker,count;bool per_worker_kind;};
struct PlacementCompare {PlacementView first,second;Z worker,node_index;U first_page,second_page,first_n,first_d,second_n,second_d;bool distinct,first_flag,second_flag;const sc::ResolvedMemoryNode* first_node;const sc::ResolvedMemoryNode* second_node;};
struct WorkCells {Z used,limit,amount,next;SV first,second;bool comparison;};
struct Arithmetic {Z left,right,result;std::optional<Z> return_value;U count,factor,product;std::optional<U> count_result;MeasuredCeiling ceiling;std::optional<MeasuredCeiling> ceiling_result;};
struct LimitsCheck {const Limits* limit;const Usage* actual;Z required,available;bool valid;};
struct ContainerLimits {std::string string_limit;std::vector<Index> index_limit;std::vector<Pair> pair_limit;};
struct Emit {const T* state;const H* input;const Pair* pair;const sel::Workload* workload;Z row,family,slot;SV token,experiment,baseline,candidate;const a::Manifest* manifest;const a::Artifact* artifact;const D* digest;std::optional<SV> token_result;};
struct SinkCall {Sink* sink;SV input;Z amount,next;char character;};
struct Decimal {std::array<char,20> buffer;U value,remaining;Z begin,end;SV formatted;MeasuredCeiling ceiling;};
struct HexTraversal {SV input;const char* current;const char* end;unsigned char byte;std::array<char,2> pair;std::span<const std::byte> digest_bytes;};
struct TokenScan {SV input,token;Z index;std::optional<SV> result;PairKind kind;sc::MatrixComparisonOrder order;};
struct FamilyTokenCalls {const sel::Workload* workload;kernel::KernelId semantic;kernel::KernelImplementationId implementation;Z index;std::optional<Z> family;SV returned;Work* work;};
struct StateConstruction {T* receiver;std::unique_ptr<T>* destination;Counts counts_argument;Usage usage_argument;D digest_argument;};
struct StrongDigestConstruction {ExecutionPlanDigest* receiver;D digest_argument;ExecutionPlanDigest returned;};
struct OwnerConstruction {P* receiver;std::unique_ptr<T>* destination;std::unique_ptr<T> argument;};
struct ReturnMove {std::unique_ptr<T> candidate;std::optional<H>* destination;H* input;H* child_receiver;Usage usage;P* result;};
struct GuardAndError {bool predicate;Error error,caught;std::unexpected<Error> returned;const std::length_error* exception;};
struct ExpectedPublication {std::expected<P,Error>* receiver;P* owner_argument;std::unexpected<Error>* failure_argument;};
struct HashCells {SV text;std::span<const std::byte> input,subspan;std::span<const char> character_span;std::array<std::uint32_t,8>* compress_state;std::array<std::uint32_t,8> state,working;std::array<std::uint32_t,64> words;std::array<std::byte,128> final_blocks;std::array<std::byte,32> result,constructor_argument;const std::byte* block;Z offset,remainder,schedule_index,schedule_offset,round_index,length_index,word,byte_index,copy_index;U bit_length;unsigned final_size;std::uint32_t s0,s1,sum0,sum1,choose,majority,temporary1,temporary2;D returned;D* constructor_receiver;};
// The heap caller's swap remains live across sift; helper argument frames are distinct.
struct SortCall {std::span<Index> index;Work* work;Index caller_swap;};
struct RootCall {Roots* output;const H* input;Work* work;};
struct CountsCall {Counts* output;const Roots* roots;Work* work;std::optional<MeasuredCeiling> ceiling;};
struct EvidenceCall {std::array<bool,2>* used;const Roots* roots;const H* input;Work* work;};
struct PairCall {T* state;const Roots* roots;std::span<const Index> index;Work* work;bool comparison,expanded;};
struct ValidationCall {const Roots* roots;const Pair* pair;Work* work;const sc::ResolvedComparisonExperiment* ordinary;const sc::ExpandedMatrixExperiment* expanded;const sc::ResolvedMatrixCell* first_cell;const sc::ResolvedMatrixCell* second_cell;};
struct PlacementCall {Placement* output;const sc::ComparisonMemoryPlacement* input;Z worker;Work* work;const Placement* first;const Placement* second;};
struct EmitCall {Sink* sink;const T* state;const Roots* roots;const H* input;const std::array<bool,2>* used;bool first;};
struct EndpointCall {Sink* sink;const Roots* roots;const Pair* pair;bool baseline;Z index;const sc::ResolvedMatrixCell* cell;};
struct ArtifactCall {Sink* sink;const H* input;SV semantic;Z size;const D* digest;};
struct LookupCall {std::span<const Index> index;Z experiment;SV key;Work* work;};
struct WorkCall {Work* receiver;Z amount;bool predicate;Error error;};
struct ArithmeticCall {Z left,right;std::optional<Z> returned;};
struct HashWorkCall {Z bytes;};
struct SiftCall {std::span<Index> index;Z root,size;Work* work;};
struct IndexCompareCall {const Index* first;const Index* second;Work* work;};
struct MemberCall {Work* work;bool required;};
struct ScalarReadCall {const Json* value;Work* work;U number;bool boolean;};
struct CountShapeCall {Usage* usage;const Roots* roots;Work* work;};
struct FlagCall {Sink* receiver;bool value;};
struct QuotedCall {Sink* receiver;SV text;};
struct DigestCall {Sink* receiver;const D* digest;const D* bytes_receiver;const std::array<std::byte,32>* bytes;};
struct HexCall {Sink* receiver;};
struct HexCountCall {Sink* receiver;Z bytes;};
struct NumberCall {Sink* receiver;};
struct CeilingCall {Sink* receiver;MeasuredCeiling value;};
} // namespace
namespace detail {
auto checked_add(Z left,Z right) noexcept -> std::optional<Z>{if(right>SIZE_MAX-left)return {};return left+right;}
auto checked_multiply(Z left,Z right) noexcept -> std::optional<Z>{if(left&&right>SIZE_MAX/left)return {};return left*right;}
auto measured_ceiling(U maximum) noexcept -> std::optional<MeasuredCeiling>{if(maximum>UINT64_MAX/4+1)return {};if(maximum==UINT64_MAX/4+1)return MeasuredCeiling{0,true};return MeasuredCeiling{maximum*4,false};}
auto valid_ceiling(MeasuredCeiling count) noexcept -> bool{return !count.two_to_64||count.low==0;}
auto order_token(sc::MatrixComparisonOrder order) noexcept -> std::optional<SV>{switch(order){case sc::MatrixComparisonOrder::randomized_abba:return "randomized-abba";case sc::MatrixComparisonOrder::randomized_baab:return "randomized-baab";case sc::MatrixComparisonOrder::randomized_balanced:return "randomized-balanced";}return {};}
auto find_name(std::span<const Index> index,Z experiment,SV key,Z& used,Z limit)->std::expected<Z,Error>{try{Work work{used,limit};need(used<=limit,Error::capacity);const auto result=lookup(index,experiment,key,work);used=work.used;return result;}catch(Error error){return std::unexpected{error};}}
auto fixed_stock() noexcept -> Z {
    return sizeof(ProduceCall)+sizeof(Roots)+sizeof(LowerUsage)+sizeof(GetterReturns)+sizeof(EvidenceScan)+sizeof(CountScan)+sizeof(JsonLookup)+sizeof(HeapSort)+sizeof(NameLookup)+sizeof(PairWalk)+sizeof(PlacementView)+sizeof(PlacementCompare)+sizeof(WorkCells)+sizeof(Arithmetic)+sizeof(LimitsCheck)+sizeof(ContainerLimits)+sizeof(Emit)+sizeof(Sink)+sizeof(SinkCall)+sizeof(Decimal)+sizeof(HexTraversal)+sizeof(TokenScan)+sizeof(FamilyTokenCalls)+sizeof(StateConstruction)+sizeof(StrongDigestConstruction)+sizeof(OwnerConstruction)+sizeof(ReturnMove)+sizeof(GuardAndError)+sizeof(ExpectedPublication)+sizeof(HashCells)+sizeof(SortCall)+sizeof(RootCall)+sizeof(CountsCall)+sizeof(EvidenceCall)+sizeof(PairCall)+sizeof(ValidationCall)+sizeof(PlacementCall)+sizeof(EmitCall)+sizeof(EndpointCall)+sizeof(ArtifactCall)+sizeof(LookupCall)+2*sizeof(WorkCall)+sizeof(ArithmeticCall)+sizeof(HashWorkCall)+sizeof(SiftCall)+sizeof(IndexCompareCall)+sizeof(MemberCall)+sizeof(ScalarReadCall)+sizeof(CountShapeCall)+sizeof(FlagCall)+sizeof(QuotedCall)+sizeof(DigestCall)+sizeof(HexCall)+sizeof(HexCountCall)+sizeof(NumberCall)+sizeof(CeilingCall)+sizeof(P)+sizeof(std::expected<P,Error>);
}
} // namespace detail

auto produce(H&& input,Limits limits)->std::expected<Owner,Error>{
    try{
        bound(detail::fixed_stock(),limits.consumer_simultaneous_bytes);
        Usage usage;usage.child_retained_bytes=input.usage().retained_bytes;usage.child_canonical_bytes=input.canonical().size();
        need(usage.child_retained_bytes>=sizeof(H),Error::state);
        bound(usage.child_retained_bytes,limits.child_retained_bytes);bound(usage.child_canonical_bytes,limits.child_canonical_bytes);
        bound(add(detail::fixed_stock(),usage.child_retained_bytes),limits.consumer_simultaneous_bytes);
        Work work{0,limits.planning_work};Roots view;roots(view,input,work);Counts counts;prepare_counts(counts,view,work);std::array<bool,2> used{};supplied_evidence(used,view,input,work);count_shape(usage,view,work);
        bound(usage.experiments,limits.experiments);bound(usage.variants,limits.variants);bound(usage.pairs,limits.pairs);
        usage.index_bytes=mul(usage.variants,sizeof(Index));usage.pair_bytes=mul(usage.pairs,sizeof(Pair));
        bound(usage.index_bytes,limits.index_bytes);bound(usage.pair_bytes,limits.pair_bytes);
        bound(add(add(detail::fixed_stock(),usage.child_retained_bytes),usage.index_bytes),limits.consumer_simultaneous_bytes);
        std::vector<Index> index;bound(usage.variants,index.max_size());index.reserve(usage.variants);
        for(Z at=0;at<view.comparisons->size();++at){work.debit();const auto& comparison=(*view.comparisons)[at];for(Z variant=0;variant<comparison.variants.size();++variant){work.debit();index.push_back({comparison.experiment_index,variant,comparison.variants[variant].variant_id});}}
        sort_index(index,work);
        usage.retained_bytes=add(add(sizeof(P),sizeof(T)),add(usage.child_retained_bytes-sizeof(H),usage.pair_bytes));
        usage.consumer_simultaneous_bytes=add(add(add(detail::fixed_stock(),usage.retained_bytes),usage.index_bytes),sizeof(H));
        bound(usage.retained_bytes,limits.retained_bytes);bound(usage.consumer_simultaneous_bytes,limits.consumer_simultaneous_bytes);bound(usage.pairs,std::vector<Pair>{}.max_size());
        std::unique_ptr<T> candidate{new T{{},{},counts,{},{},ExecutionPlanDigest{*input.digest()},*input.digest(),{}}};candidate->pairs.reserve(usage.pairs);pairs(*candidate,view,index,work);work.test(candidate->pairs.size()==usage.pairs,Error::reference);
        const auto work_after_pairs=work.used;Sink sink{nullptr,0,limits.semantic_bytes,work};semantic(sink,*candidate,view,input,used);usage.semantic_bytes=sink.size;
        sink.size=0;sink.limit=limits.canonical_bytes;artifact(sink,input,{},usage.semantic_bytes,*input.digest());usage.canonical_bytes=sink.size;
        const auto remaining_work=add(work.used-work_after_pairs,add(hash_work(usage.semantic_bytes),hash_work(usage.canonical_bytes)));
        usage.planning_work=add(work.used,remaining_work);bound(usage.planning_work,limits.planning_work);
        usage.retained_bytes=add(usage.retained_bytes,add(usage.semantic_bytes,usage.canonical_bytes));usage.consumer_simultaneous_bytes=add(usage.consumer_simultaneous_bytes,add(usage.semantic_bytes,usage.canonical_bytes));
        final_bounds(usage,limits);bound(usage.semantic_bytes,std::string{}.max_size());bound(usage.canonical_bytes,std::string{}.max_size());
        candidate->semantic.reserve(usage.semantic_bytes);candidate->canonical.reserve(usage.canonical_bytes);
        sink.output=&candidate->semantic;sink.size=0;sink.limit=usage.semantic_bytes;semantic(sink,*candidate,view,input,used);need(sink.size==usage.semantic_bytes,Error::encoding);
        work.debit(hash_work(usage.semantic_bytes));candidate->execution_digest=ExecutionPlanDigest{core::sha256(candidate->semantic)};
        sink.output=&candidate->canonical;sink.size=0;sink.limit=usage.canonical_bytes;artifact(sink,input,candidate->semantic,usage.semantic_bytes,candidate->execution_digest.value());need(sink.size==usage.canonical_bytes,Error::encoding);
        work.debit(hash_work(usage.canonical_bytes));candidate->digest=core::sha256(candidate->canonical);need(work.used==usage.planning_work,Error::encoding);
        std::vector<Index>{}.swap(index);candidate->usage=usage;candidate->child.emplace(std::move(input));return Owner{std::move(candidate)};
    }catch(Error error){return std::unexpected{error};}catch(const std::length_error&){return std::unexpected{Error::capacity};}
}
Owner::Owner(std::unique_ptr<T> state) noexcept:state_{std::move(state)}{}
Owner::Owner(Owner&&) noexcept=default;auto Owner::operator=(Owner&&) noexcept->Owner& = default;Owner::~Owner()=default;
auto Owner::evidence() const noexcept->const H*{return state_&&state_->child?&*state_->child:nullptr;}
auto Owner::pairs() const noexcept->std::span<const Pair>{return state_?std::span<const Pair>{state_->pairs}:std::span<const Pair>{};}
auto Owner::counts() const noexcept->const Counts*{return state_?&state_->counts:nullptr;}
auto Owner::measurement() const noexcept->const Json*{if(!evidence())return nullptr;const auto* root=evidence()->selection()->normalization()->plan()->document_ancestry()->default_expanded();const auto found=root->find("measurement");return found==root->end()?nullptr:&*found;}
auto Owner::monitoring() const noexcept->const Json*{if(!evidence()||!state_->counts.monitoring_present)return nullptr;const auto* root=evidence()->selection()->normalization()->plan()->document_ancestry()->default_expanded();const auto found=root->find("monitoring");return found==root->end()?nullptr:&*found;}
auto Owner::semantic() const noexcept->SV{return state_?SV{state_->semantic}:SV{};}
auto Owner::canonical() const noexcept->SV{return state_?SV{state_->canonical}:SV{};}
auto Owner::execution_digest() const noexcept->const ExecutionPlanDigest*{return state_?&state_->execution_digest:nullptr;}
auto Owner::digest() const noexcept->const D*{return state_?&state_->digest:nullptr;}
auto Owner::usage() const noexcept->Usage{return state_?state_->usage:Usage{};}
} // namespace epycbench::results::execution_plan
