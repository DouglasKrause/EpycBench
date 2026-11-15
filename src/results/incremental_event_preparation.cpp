#include "results/incremental_event_preparation.hpp"
#include "results/incremental_event_profile.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "runtime/scenario_monitoring_policy_projection.hpp"
#include "runtime/production_admission_policy.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <charconv>

namespace epycbench::results::event_profile::detail {
namespace {
using Json=nlohmann::json;
using SV=std::string_view;
namespace selection=implementation_selection;
namespace plan=execution_plan;
namespace association=build_object_association;
constexpr std::array semantics{kernel::KernelId::sequential_read_bandwidth_v1,kernel::KernelId::dependent_load_latency_v1};
constexpr std::array implementations{kernel::KernelImplementationId::scalar_u64_sequential_v1,kernel::KernelImplementationId::scalar_u64_random_cycle_v1};
constexpr std::array<SV,2> compilers{"GCC","Clang"},presets{"release","release-clang"};
void fields(const Json& j,std::initializer_list<SV> names) {
    require(j.is_object()&&j.size()==names.size(),Error::schema);
    for(const auto name:names)require(j.contains(name),Error::schema);
}
auto text(const Json& j)->const std::string& {
    require(j.is_string(),Error::schema);return j.get_ref<const std::string&>();
}
auto decimal(const Json& j)->std::uint64_t {
    const auto& s=text(j);require(!s.empty()&&(s.size()==1U||s.front()!='0'),Error::schema);
    std::uint64_t value{};const auto result=std::from_chars(s.data(),s.data()+s.size(),value);
    require(result.ec==std::errc{}&&result.ptr==s.data()+s.size(),Error::schema);return value;
}
auto hex(SV bytes,Work& work)->std::string {
    work.debit(multiply(bytes.size(),2U));std::string result;result.reserve(multiply(bytes.size(),2U));
    constexpr SV digits="0123456789abcdef";
    for(const char character:bytes){const auto value=static_cast<unsigned char>(character);result.push_back(digits[value>>4U]);result.push_back(digits[value&15U]);}
    return result;
}
auto unhex(const Json& j,Work& work)->std::string {
    const auto& s=text(j);require(s.size()%2U==0U,Error::schema);work.debit(s.size());
    auto nibble=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');
        if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);
        throw Error::schema;};
    std::string result;result.resize(s.size()/2U);
    for(std::size_t i{};i<result.size();++i)result[i]=static_cast<char>((nibble(s[2U*i])<<4U)|nibble(s[2U*i+1U]));
    return result;
}
auto digest(SV bytes,Work& work)->core::Sha256Digest {
    require(bytes.size()<=UINT64_MAX/8U,Error::capacity);
    work.debit(add(bytes.size(),multiply(64U,add(bytes.size()/64U,bytes.size()%64U>=56U?2U:1U))));
    return core::sha256(bytes);
}
void same_digest(const Json& j,const core::Sha256Digest& expected,Work& work) {
    require(work.equal(text(j),expected.to_hex()),Error::digest);
}
template<class Bounds>
auto parse(SV bytes,const Bounds& bound,Work& work)->Json {
    Limits limits;limits.body_bytes=bound.bytes;limits.nodes=bound.nodes;limits.depth=bound.depth;limits.text_bytes=bound.text;limits.candidate_bytes=bound.simultaneous;
    auto value=parse_tree(bytes,limits,work);core::detail::JsonCleanup tree_cleanup{value};
    auto canonical=core::canonicalize_json_jcs(value,{bound.bytes,bound.depth,bound.nodes});
    core::detail::ExpectedErrorCleanup cleanup{canonical};
    require(canonical.has_value()&&work.equal(bytes,*canonical),Error::canonical);return Json(std::move(value));
}
template<class Bounds>
void same(const Json& actual,const Json& expected,const Bounds& bound,Work& work) {
    // Both trees have already bounded dimensions. Charge all possible compared
    // cells and octets before the library's recursive equality traversal.
    work.debit(add(multiply(bound.nodes,2U),multiply(bound.text,2U)));
    require(actual==expected,Error::association);
}
auto family(const selection::Workload& workload)->std::size_t {
    const auto index=association::detail::family_index(workload.semantic,workload.implementation);
    require(index.has_value(),Error::association);return *index;
}
void selection_facts(PlanFacts& out,const Json& value,const normalization_serialization::Owner& normalization,
    const PlanLimits& bound,Work& work) {
    fields(value,{"bindings","normalization_digest","normalization_hex","resolved_plan_digest","scenario_digest","schema","workloads"});
    const bool plugins=text(value.at("schema"))==selection::plugin_profile;
    require(plugins||text(value.at("schema"))==selection::profile,Error::schema);
    const auto* normalized=normalization.plan();require(normalized&&normalization.digest(),Error::child);
    require(work.equal(unhex(value.at("normalization_hex"),work),normalization.canonical()),Error::association);
    same_digest(value.at("normalization_digest"),*normalization.digest(),work);
    same_digest(value.at("resolved_plan_digest"),normalized->resolved_digest()->digest(),work);
    same_digest(value.at("scenario_digest"),normalized->scenario_digest()->digest(),work);
    const auto* dispatch=normalized->kernel_dispatch();require(dispatch&&dispatch->workloads()&&dispatch->experiment_workloads(),Error::child);
    const auto& workloads=*dispatch->workloads();const auto& bindings=*dispatch->experiment_workloads();
    require(workloads.size()<=bound.workloads&&bindings.size()<=bound.bindings,Error::capacity);
    require(value.at("workloads").is_array()&&value.at("workloads").size()==workloads.size()&&
        value.at("bindings").is_array()&&value.at("bindings").size()==bindings.size(),Error::association);
    std::size_t text_bytes{};
    for(const auto& [id,source]:workloads){(void)source;text_bytes=add(text_bytes,id.size());}
    require(text_bytes<=bound.text,Error::capacity);
    out.workloads.reserve(workloads.size());out.bindings.reserve(bindings.size());
    for(const auto& [id,source]:workloads){
        work.debit(add(1U,id.size()));
        const auto& row=value.at("workloads")[out.workloads.size()];
        require(row.is_array()&&row.size()==(plugins?4U:3U),Error::schema);
        require(work.equal(text(row[0]),hex(id,work))&&
            work.equal(text(row[1]),kernel::kernel_id_token(source.kernel_id)),Error::association);
        const auto implementation=kernel::parse_kernel_implementation_id(text(row[2]),source.kernel_id.family());
        require(implementation.has_value(),Error::association);
        if(!plugins)require(*implementation==(source.kernel_id.family()==kernel::KernelFamily::bandwidth?
            implementations[0]:implementations[1]),Error::association);
        std::optional<kernel::ModuleIdentity> module;
        if(plugins&&!row[3].is_null()) {
            fields(row[3],{"build_info_hex","family","file_sha256","version_hex"});
            require(text(row[3].at("family"))==(source.kernel_id.family()==kernel::KernelFamily::bandwidth?"bandwidth":"latency"),Error::association);
            module.emplace(kernel::ModuleIdentity{unhex(row[3].at("version_hex"),work),unhex(row[3].at("build_info_hex"),work),text(row[3].at("file_sha256"))});
            require(selection::detail::valid_module(*module),Error::schema);
            text_bytes=add(text_bytes,add(add(module->version.size(),module->build_info.size()),module->file_sha256.size()));
            require(text_bytes<=bound.text,Error::capacity);
        }
        out.workloads.push_back({id,source.kernel_id,*implementation,std::move(module)});
    }
    require(plugins==selection::detail::uses_plugins(out.workloads),Error::schema);
    for(const auto& source:bindings){
        work.debit(1U);const auto selected=selection::detail::find_workload(out.workloads,source.workload_id,work.used,work.limit);
        require(selected.has_value(),Error::association);
        const auto& row=value.at("bindings")[out.bindings.size()];
        Json expected=Json::array();core::detail::JsonCleanup cleanup{expected};
        expected.push_back(std::to_string(source.experiment_index));expected.push_back(std::to_string(*selected));same(row,expected,bound,work);
        out.bindings.push_back({source.experiment_index,*selected});
    }
}
auto report_text(const kernel_report::Value& value,SV group,SV field)->SV {
    const auto* parent=value.member(group);require(parent&&parent->kind()==kernel_report::Value::Kind::object,Error::child);
    const auto* child=parent->member(field);require(child,Error::child);return child->text();
}
auto approval_route(SV bytes,const PlanLimits& bound,Work& work)->kernel_approval::ReviewRoute {
    Limits limits;
    limits.body_bytes=std::min(bound.bytes,bound.approval.input_bytes);
    limits.nodes=std::min(bound.nodes,bound.approval.nodes);
    limits.depth=std::min(bound.depth,bound.approval.depth);
    limits.text_bytes=std::min(bound.text,bound.approval.string_bytes);
    limits.candidate_bytes=bound.simultaneous;
    // Read only the declared version; raw evidence need not be canonical. The
    // bounded duplicate-rejecting tree retires before any child is consumed.
    auto value=parse_tree(bytes,limits,work);core::detail::JsonCleanup cleanup{value};
    require(value.is_object()&&value.contains("schema"),Error::schema);
    const auto& schema=text(value.at("schema"));
    if(work.equal(schema,"epycbench.kernel-object-qualification-approval/v1"))
        return kernel_approval::ReviewRoute::human_approval;
    require(work.equal(schema,"epycbench.kernel-object-agent-qualification/v1"),Error::schema);
    return kernel_approval::ReviewRoute::independent_agents;
}
void build_facts(PlanFacts& out,const Json& value,const normalization_serialization::Owner& normalization,
    const PlanLimits& bound,Work& work) {
    fields(value,{"base_build_digest","base_build_hex","build_manifest_digest","families",
        "implementation_selection_digest","implementation_selection_hex","schema"});
    const bool plugins=text(value.at("schema"))==association::plugin_profile;
    require(plugins||text(value.at("schema"))==association::profile,Error::schema);
    const auto selected_bytes=unhex(value.at("implementation_selection_hex"),work);
    same_digest(value.at("implementation_selection_digest"),digest(selected_bytes,work),work);
    auto selected_tree=parse(selected_bytes,bound,work);core::detail::JsonCleanup selected_cleanup{selected_tree};
    selection_facts(out,selected_tree,normalization,bound,work);
    require(plugins==selection::detail::uses_plugins(out.workloads),Error::association);
    const auto base_bytes=unhex(value.at("base_build_hex"),work);
    auto base=base_build_serialization::Owner::decode(base_bytes,bound.base);require(base.has_value(),Error::child);
    same_digest(value.at("base_build_digest"),*base->digest(),work);
    require(base->provenance()&&normalization.provenance()&&base->manifest()&&base->manifest()->digest()&&
        base->disposition()==process_subject::BuildState::available,Error::association);
    const auto& provenance=*base->provenance();const auto& original=*normalization.provenance();
    require(work.equal(provenance.run,original.run)&&provenance.domain==original.domain&&provenance.sequence&&original.sequence&&
        (provenance.sequence!=original.sequence||work.equal(provenance.time,original.time)),Error::association);
    same_digest(value.at("build_manifest_digest"),base->manifest()->digest()->value(),work);
    out.base.emplace(std::move(*base));
    const auto build=out.base->manifest()->input();require(build.has_value(),Error::child);
    require(value.at("families").is_array()&&value.at("families").size()==2U,Error::schema);
    std::array<std::size_t,2> declarations{},bindings{};
    for(const auto& workload:out.workloads){work.debit(1U);++declarations[family(workload)];}
    for(const auto& binding:out.bindings){work.debit(1U);++bindings[family(out.workloads[binding.workload_index])];}
    for(std::size_t f{};f<2U;++f){
        work.debit(1U);const auto& row=value.at("families")[f];
        if(plugins) {
            fields(row,{"approval","bindings","compiler_slots","declared_workloads","family"});
            require(text(row.at("family"))==(f==0U?"bandwidth":"latency"),Error::association);
        } else {
            fields(row,{"approval","bindings","compiler_slots","declared_workloads","implementation_id","semantic_id"});
            require(text(row.at("implementation_id"))==kernel::kernel_implementation_id_token(implementations[f])&&
                text(row.at("semantic_id"))==kernel::kernel_id_token(semantics[f]),Error::association);
        }
        require(decimal(row.at("bindings"))==bindings[f]&&decimal(row.at("declared_workloads"))==declarations[f],Error::association);
        const auto& approval=row.at("approval");
        if(!approval.is_null()){
            fields(approval,{"digest","hex","human_hex","machine_hex"});
            require(approval.at("human_hex").is_array()&&approval.at("human_hex").size()==2U&&
                approval.at("machine_hex").is_array()&&approval.at("machine_hex").size()==2U,Error::schema);
            const std::array human{unhex(approval.at("human_hex")[0],work),unhex(approval.at("human_hex")[1],work)};
            const std::array machine{unhex(approval.at("machine_hex")[0],work),unhex(approval.at("machine_hex")[1],work)};
            auto gcc=kernel_report::retain(machine[0],bound.report);core::detail::ExpectedErrorCleanup gcc_cleanup{gcc};
            auto clang=kernel_report::retain(machine[1],bound.report);core::detail::ExpectedErrorCleanup clang_cleanup{clang};
            require(gcc.has_value()&&clang.has_value(),Error::child);
            const auto approval_bytes=unhex(approval.at("hex"),work);
            const auto route=approval_route(approval_bytes,bound,work);
            const auto retain=route==kernel_approval::ReviewRoute::independent_agents?
                kernel_approval::retain_agent:kernel_approval::retain;
            auto retained=retain(approval_bytes,std::move(*gcc),std::move(*clang),
                {human[0],human[1]},bound.approval);core::detail::ExpectedErrorCleanup approval_cleanup{retained};
            require(retained.has_value()&&retained->manifest(),Error::child);
            same_digest(approval.at("digest"),*retained->digest(),work);
            require(work.equal(retained->manifest()->implementation_id,kernel::kernel_implementation_id_token(implementations[f])),Error::association);
            out.approvals[f].emplace(std::move(*retained));
        }
        require(row.at("compiler_slots").is_array()&&row.at("compiler_slots").size()==2U,Error::schema);
        for(std::size_t slot{};slot<2U;++slot){
            work.debit(1U);std::array<association::Match,7> comparisons;comparisons.fill(association::Match::not_available);
            Json expected=Json::object();core::detail::JsonCleanup expected_cleanup{expected};
            expected["compiler_family"]=compilers[slot];expected["function_sha256"]=nullptr;expected["object_sha256"]=nullptr;
            expected["qualification_preset"]=presets[slot];expected["comparison"]=Json::array();
            if(out.approvals[f]){
                const auto* owner=out.approvals[f]->report(slot);require(owner,Error::child);
                const auto report=owner->report();require(report.has_value(),Error::child);
                require(report->family==(f==0U?kernel_report::Family::bandwidth:kernel_report::Family::latency),Error::association);
                const auto& content=report->content;const auto& manifest=*out.approvals[f]->manifest();
                const auto revision=report_text(content,"source","commit");
                const auto family_name=report_text(content,"compiler","family");
                const auto preset=report_text(content,"qualification","preset");
                const auto object=report_text(content,"object","sha256");const auto function=report_text(content,"function","sha256");
                require(work.equal(revision,manifest.source_commit)&&report_text(content,"source","dirty")=="false"&&
                    family_name==compilers[slot]&&preset==presets[slot]&&work.equal(object,manifest.artifacts[slot].object_sha256)&&
                    work.equal(function,manifest.artifacts[slot].function_sha256),Error::association);
                const auto compiler=association::detail::compiler_family(build->compiler.id);
                comparisons[0]=compiler?(static_cast<std::size_t>(*compiler)==slot?association::Match::equal:association::Match::different):association::Match::not_comparable;
                comparisons[1]=work.equal(build->compiler.target,report_text(content,"compiler","target"))?association::Match::equal:association::Match::different;
                comparisons[2]=work.equal(build->compiler.version,report_text(content,"compiler","version"))?association::Match::equal:association::Match::different;
                comparisons[3]=association::Match::not_comparable;
                comparisons[4]=work.equal(build->preset,preset)?association::Match::equal:association::Match::different;
                comparisons[5]=build->git.dirty?association::Match::different:association::Match::equal;
                comparisons[6]=work.equal(build->git.revision,revision)?association::Match::equal:association::Match::different;
                expected["object_sha256"]=object;expected["function_sha256"]=function;
            }
            for(const auto comparison:comparisons)expected["comparison"].push_back(*association::detail::match_token(comparison));
            same(row.at("compiler_slots")[slot],expected,bound,work);
        }
    }
}
struct PlacementView {
    const scenario::ResolvedMemoryPlacement* uniform{};
    const scenario::ResolvedPerWorkerMemoryPlacement* per_worker{};
    std::span<const scenario::ResolvedMemoryNode> nodes;
};
auto placement(const scenario::ComparisonMemoryPlacement& value,std::size_t worker)->PlacementView {
    if(const auto* uniform=std::get_if<scenario::ResolvedMemoryPlacement>(&value))return {uniform,nullptr,uniform->nodes};
    const auto* per_worker=std::get_if<scenario::ResolvedPerWorkerMemoryPlacement>(&value);
    require(per_worker&&worker<per_worker->worker_nodes.size(),Error::association);
    return {nullptr,per_worker,{&per_worker->worker_nodes[worker].node,1U}};
}
void placement_pair(PlacementView first,PlacementView second,bool& distinct,Work& work) {
    work.debit(add(1U,add(first.nodes.size(),second.nodes.size())));
    const auto compare=[&](const auto& a,const auto& b){
        require(a.page_mode==b.page_mode&&a.page_size==b.page_size&&a.placement_threshold==b.placement_threshold&&
            a.prefault==b.prefault&&a.verify_before==b.verify_before&&a.verify_after==b.verify_after,Error::association);
        if(a.policy!=b.policy||first.nodes.size()!=second.nodes.size()){distinct=true;return;}
        for(std::size_t i{};i<first.nodes.size();++i)if(first.nodes[i].linux_node!=second.nodes[i].linux_node)distinct=true;
    };
    if(first.uniform){if(second.uniform)compare(*first.uniform,*second.uniform);else compare(*first.uniform,*second.per_worker);}
    else{if(second.uniform)compare(*first.per_worker,*second.uniform);else compare(*first.per_worker,*second.per_worker);}
}
void semantic_facts(PlanFacts& out,const Json& value,const normalization_serialization::Owner& normalization,
    const PlanLimits& bound,Work& work) {
    const bool plugins=selection::detail::uses_plugins(out.workloads);
    const auto* normalized=normalization.plan();
    const auto* ancestry=normalized->document_ancestry();const auto* comparison=normalized->comparison_experiments();
    const auto* matrix=normalized->matrix_experiments();
    require(ancestry&&ancestry->default_expanded()&&comparison&&comparison->experiments()&&matrix&&matrix->experiments(),Error::child);
    const auto& effective=*ancestry->default_expanded();const auto& measurement=effective.at("measurement");
    const auto& repetition=measurement.at("repetition");auto& counts=out.counts;
    auto number=[](const auto& v){require(v.is_number_unsigned(),Error::association);return v.template get<std::uint64_t>();};
    counts.root_seed=number(effective.at("random").at("seed"));
    counts.minimum_blocks=number(repetition.at("minimum_blocks"));counts.maximum_blocks=number(repetition.at("maximum_blocks"));
    counts.minimum_valid_per_variant=number(repetition.at("minimum_valid_observations_per_variant"));
    const auto ceiling=plan::detail::measured_ceiling(counts.maximum_blocks);
    require(ceiling.has_value()&&counts.minimum_blocks<=counts.maximum_blocks,Error::association);
    counts.maximum_observations=*ceiling;counts.maximum_per_variant=counts.maximum_blocks*2U;
    require(measurement.at("stopping").at("enabled").is_boolean(),Error::association);
    counts.stopping_enabled=measurement.at("stopping").at("enabled").get<bool>();counts.monitoring_present=effective.contains("monitoring");
    Json expected=Json::object();core::detail::JsonCleanup expected_cleanup{expected};
    expected["build_manifest_digest"]=out.base->manifest()->digest()->value().to_hex();
    for(const auto key:{"implementation_evidence","implementations","pairs"})expected[key]=Json::array();
    expected["monitoring_presence"]=counts.monitoring_present?"supplied":"not_supplied";
    auto& policy=expected["policy"];policy["build_evidence"]=plugins?"build-manifest-and-recorded-modules":out.bindings.empty()||out.approvals[0]||out.approvals[1]?"supplied-manifest-and-bound-family-objects":"build-manifest";
    policy["current_qualification"]="not_asserted";policy["production_admission"]="deferred";policy["schedule"]="measurement-four-slot-v1";
    auto& repeat=expected["repetition"];repeat["actual_completed"]="unknown";repeat["maximum_blocks"]=std::to_string(counts.maximum_blocks);
    repeat["maximum_measured_observations"]=ceiling->two_to_64?"18446744073709551616":std::to_string(ceiling->low);
    repeat["maximum_per_variant"]=std::to_string(counts.maximum_per_variant);repeat["minimum_blocks"]=std::to_string(counts.minimum_blocks);
    repeat["minimum_valid_per_variant"]=std::to_string(counts.minimum_valid_per_variant);repeat["stopping_enabled"]=counts.stopping_enabled;
    expected["resolved_plan_digest"]=normalized->resolved_digest()->digest().to_hex();
    expected["scenario_digest"]=normalized->scenario_digest()->digest().to_hex();expected["schema"]=plugins?plan::plugin_profile:plan::profile;
    auto& seed=expected["seed_intent"];seed["root_seed"]=std::to_string(counts.root_seed);auto& bootstrap=seed["bootstrap"];
    bootstrap["components"]=Json::array();bootstrap["derivation"]="deferred-to-production-policy";
    bootstrap["domain"]="bootstrap";bootstrap["profile"]="epycbench.seed-derivation.v1";
    std::array<bool,2> used{};
    for(const auto& binding:out.bindings){work.debit(1U);used[family(out.workloads[binding.workload_index])]=true;}
    for(std::size_t f{};f<2U;++f){
        if(!used[f]||!out.approvals[f])continue;
        require(out.approvals[f]->manifest(),Error::association);
        const auto& manifest=*out.approvals[f]->manifest();
        core::detail::append_object_json(expected["implementation_evidence"],[&](Json& row){
            row["framework_version"]=hex(manifest.framework_version,work);
            row["implementation_id"]=kernel::kernel_implementation_id_token(implementations[f]);
            row["policy_version"]=hex(manifest.policy_version,work);row["report_schema"]=hex(manifest.report_schema,work);
            row["semantic_id"]=kernel::kernel_id_token(semantics[f]);row["source_commit"]=hex(manifest.source_commit,work);row["slots"]=Json::array();
            for(std::size_t slot{};slot<2U;++slot)core::detail::append_object_json(row["slots"],[&](Json& entry){
                entry["compiler_family"]=compilers[slot];entry["function_sha256"]=manifest.artifacts[slot].function_sha256;
                entry["object_sha256"]=manifest.artifacts[slot].object_sha256;entry["qualification_preset"]=presets[slot];});
        });
    }
    for(const auto& workload:out.workloads){
        Json row=Json::array();core::detail::JsonCleanup row_cleanup{row};row.push_back(hex(workload.workload_id,work));
        row.push_back(kernel::kernel_id_token(workload.semantic));row.push_back(kernel::kernel_implementation_id_token(workload.implementation));
        expected["implementations"].push_back(std::move(row));
    }
    if(plugins) {
        expected["modules"]=Json::array();
        for(const auto& workload:out.workloads) {
            if(!workload.module){expected["modules"].push_back(nullptr);continue;}
            core::detail::append_object_json(expected["modules"],[&](Json& row){
                row["build_info_hex"]=hex(workload.module->build_info,work);
                row["family"]=workload.semantic.family()==kernel::KernelFamily::bandwidth?"bandwidth":"latency";
                row["file_sha256"]=workload.module->file_sha256;
                row["implementation_id"]=kernel::kernel_implementation_id_token(workload.implementation);
                row["kernel_id"]=kernel::kernel_id_token(workload.semantic);
                row["version_hex"]=hex(workload.module->version,work);row["workload_hex"]=hex(workload.workload_id,work);
            });
        }
    }
    const auto& ordinary=*comparison->experiments();const auto& matrices=*matrix->experiments();
    std::size_t pair_count{};
    for(const auto& source:ordinary)pair_count=add(pair_count,source.candidate_variant_ids.size());
    for(const auto& source:matrices)pair_count=add(pair_count,source.comparison_edges.size());
    require(pair_count<=bound.pairs,Error::capacity);out.pairs.reserve(pair_count);
    std::size_t ordinary_index{},matrix_index{};
    auto publish_pair=[&](plan::Pair pair,SV experiment,const std::array<SV,2>& baseline,const std::array<SV,2>& candidate,std::size_t keys){
        require(out.pairs.size()<bound.pairs,Error::capacity);pair.ordinal=out.pairs.size();
        const auto order=plan::detail::order_token(pair.order);require(order.has_value(),Error::association);
        core::detail::append_object_json(expected["pairs"],[&](Json& row){
            row["baseline"]=Json::array();row["candidate"]=Json::array();
            for(std::size_t i{};i<keys;++i){row["baseline"].push_back(hex(baseline[i],work));row["candidate"].push_back(hex(candidate[i],work));}
            row["experiment"]=hex(experiment,work);row["experiment_index"]=std::to_string(pair.experiment_index);
            row["kind"]=pair.kind==plan::PairKind::comparison?"comparison":"matrix";row["order"]=*order;
            row["order_seed"]=std::to_string(pair.order_seed);row["ordinal"]=std::to_string(pair.ordinal);row["workload_index"]=std::to_string(pair.workload_index);
        });
        out.pairs.push_back(pair);
    };
    for(std::size_t experiment{};experiment<out.bindings.size();++experiment){
        work.debit(1U);const auto& binding=out.bindings[experiment];require(binding.experiment_index==experiment,Error::association);
        const bool ordinary_match=ordinary_index<ordinary.size()&&ordinary[ordinary_index].experiment_index==experiment;
        const bool matrix_match=matrix_index<matrices.size()&&matrices[matrix_index].experiment_index==experiment;
        require(ordinary_match!=matrix_match,Error::association);
        const auto& configuration=ordinary_match?ordinary[ordinary_index].comparison:matrices[matrix_index].comparison;
        plan::Pair pair{0U,experiment,ordinary_match?ordinary_index:matrix_index,binding.workload_index,0U,0U,
            ordinary_match?plan::PairKind::comparison:plan::PairKind::matrix,configuration.order,configuration.order_seed.value_or(counts.root_seed)};
        if(ordinary_match){
            const auto& source=ordinary[ordinary_index];
            auto lookup=[&](SV id){std::optional<std::size_t> found;
                for(std::size_t i{};i<source.variants.size();++i)if(work.equal(source.variants[i].variant_id,id)){
                    require(!found,Error::association);found=i;}
                require(found.has_value(),Error::association);return *found;};
            pair.baseline_index=lookup(source.baseline_variant_id);
            for(const auto& candidate:source.candidate_variant_ids){
                pair.candidate_index=lookup(candidate);require(pair.baseline_index!=pair.candidate_index,Error::association);
                bool distinct=false;
                for(std::size_t worker{};worker<source.workers.size();++worker)placement_pair(
                    placement(source.variants[pair.baseline_index].memory_placement,worker),
                    placement(source.variants[pair.candidate_index].memory_placement,worker),distinct,work);
                require(distinct,Error::association);
                publish_pair(pair,source.experiment_id,{source.baseline_variant_id,{}},{candidate,{}},1U);
            }
            ++ordinary_index;
        }else{
            const auto& source=matrices[matrix_index];
            for(const auto& edge:source.comparison_edges){
                work.debit(1U);pair.baseline_index=edge.baseline_cell_index;pair.candidate_index=edge.candidate_cell_index;
                require(pair.baseline_index<source.cells.size()&&pair.candidate_index<source.cells.size()&&pair.baseline_index!=pair.candidate_index,Error::association);
                const auto& first=source.cells[pair.baseline_index];const auto& second=source.cells[pair.candidate_index];
                require(first.worker_row_index==second.worker_row_index,Error::association);bool distinct=false;
                placement_pair({&first.memory_placement,nullptr,first.memory_placement.nodes},
                    {&second.memory_placement,nullptr,second.memory_placement.nodes},distinct,work);require(distinct,Error::association);
                publish_pair(pair,source.experiment_id,{first.id.worker_node.value(),first.id.memory_node.value()},
                    {second.id.worker_node.value(),second.id.memory_node.value()},2U);
            }
            ++matrix_index;
        }
    }
    require(ordinary_index==ordinary.size()&&matrix_index==matrices.size(),Error::association);
    same(value,expected,bound,work);
}
} // namespace

auto decode_plan(SV bytes,const normalization_serialization::Owner& normalization,const PlanLimits& bound,Work& work,
    std::size_t canonical_overlap)->PlanFacts {
    require(bound.depth&&bound.depth<=64U&&bytes.size()<=bound.bytes,Error::capacity);
    Limits generic;generic.body_bytes=bound.bytes;generic.nodes=bound.nodes;generic.depth=bound.depth;generic.text_bytes=bound.text;generic.candidate_bytes=bound.simultaneous;
    (void)shape(bytes,generic,work);
    const Shape dimensions{bound.nodes,bound.nodes,bound.text,bound.depth,bound.bytes};
    // The complete outer, evidence, selection and semantic trees plus their
    // projections coexist. Eight bounded tree workspaces cover both input and
    // derived trees; typed child configured allowances are admitted separately.
    const auto trees=multiply(workspace(dimensions),8U);
    const auto rows=add(add(multiply(bound.workloads,sizeof(selection::Workload)),multiply(bound.bindings,sizeof(selection::Binding))),
        multiply(bound.pairs,sizeof(plan::Pair)));
    const auto children=add(bound.base.publication_bytes,add(multiply(bound.approval.retained_bytes,2U),
        add(multiply(bound.report.retained_bytes,2U),multiply(bound.approval.scanner_bytes,2U))));
    // Derived projections can repeat retained endpoint names across pairs. Their
    // source octets are already embedded in the bounded outer evidence, but a
    // deliberately short false semantic claim must not undercharge that replay.
    const auto projection_rows=add(add(bound.workloads,bound.bindings),add(bound.pairs,16U));
    const Shape projection{multiply(projection_rows,64U),multiply(projection_rows,64U),
        multiply(bytes.size(),multiply(projection_rows,2U)),bound.depth,0U};
    const auto peak=add(canonical_overlap,add(sizeof(PlanFacts),add(trees,add(rows,add(children,json_stock(projection))))));
    require(peak<=bound.simultaneous,Error::capacity);work.reserve_peak(peak);
    work.debit(add(children,add(projection.nodes,projection.text)));
    auto outer=parse(bytes,bound,work);core::detail::JsonCleanup outer_cleanup{outer};
    fields(outer,{"evidence_digest","evidence_hex","execution_plan_digest","schema","semantic_hex"});
    require(text(outer.at("schema"))==plan::artifact_profile,Error::schema);
    auto evidence=unhex(outer.at("evidence_hex"),work);auto semantic=unhex(outer.at("semantic_hex"),work);
    const auto evidence_digest=digest(evidence,work),semantic_digest=digest(semantic,work);
    same_digest(outer.at("evidence_digest"),evidence_digest,work);same_digest(outer.at("execution_plan_digest"),semantic_digest,work);
    PlanFacts result{std::string{bytes},std::move(semantic),digest(bytes,work),semantic_digest,{},{},{},{},{},{},0U,0U};
    {auto evidence_tree=parse(evidence,bound,work);core::detail::JsonCleanup cleanup{evidence_tree};
        build_facts(result,evidence_tree,normalization,bound,work);}
    {auto semantic_tree=parse(result.semantic,bound,work);core::detail::JsonCleanup cleanup{semantic_tree};
        semantic_facts(result,semantic_tree,normalization,bound,work);}
    auto retained=add(sizeof(PlanFacts),add(result.canonical.size(),result.semantic.size()));
    retained=add(retained,result.base->usage().retained_bytes);
    for(const auto& approval:result.approvals)if(approval)retained=add(retained,approval->usage().retained_bytes);
    retained=add(retained,add(multiply(result.workloads.capacity(),sizeof(selection::Workload)),
        add(multiply(result.bindings.capacity(),sizeof(selection::Binding)),multiply(result.pairs.capacity(),sizeof(plan::Pair)))));
    for(const auto& row:result.workloads) {
        retained=add(retained,row.workload_id.size());
        if(row.module)retained=add(retained,add(add(row.module->version.size(),row.module->build_info.size()),row.module->file_sha256.size()));
    }
    require(retained<=bound.retained&&retained<=bound.simultaneous,Error::capacity);
    result.retained=retained;result.simultaneous=peak;return result;
}
namespace {
namespace pf=preflight;
constexpr std::array<SV,34> check_names{"capture_model","capture_signature","capture_online","capture_numa","capture_cores",
    "capture_caches","capture_memory","capture_kernel","capture_build","capture_compiler","capture_frequency","capture_controls",
    "capture_anonymous_thp","capture_process_thp","capture_scenario","operating_system","architecture","cpu_vendor","cpu_nodes",
    "memory_nodes","smt_permitted","required_feature","preferred_feature","numa_balancing","cpu_exclusivity","unintended_smt",
    "hierarchy","monitoring_policy","monitoring_channel","thermal_observation","thermal_suitability","frequency_difference","placement_intent","service_core"};
constexpr std::array<SV,2> evaluation_names{"not_evaluated","evaluated"};
constexpr std::array<SV,5> outcome_names{"pass","fail","warning","unavailable","not_applicable"};
constexpr std::array<SV,10> reason_names{"none","not_attempted","source_unavailable","source_failed","unknown_fact",
    "conflicting_fact","missing_policy","unsupported_rule","policy_failure","pending_preparation"};
constexpr std::array<SV,4> thermal_names{"not_attempted","available","unavailable","failed"};
constexpr std::array<SV,6> thermal_reason_names{"not_supported","source_missing","access_denied","read_failed","malformed_source","unsupported_value"};
constexpr std::array<SV,4> status_names{"accepted","accepted_with_warnings","incomplete","rejected"};
constexpr std::array<SV,4> cache_names{"data","instruction","unified","unknown"};
template<class E,std::size_t N> auto token(E value,const std::array<SV,N>& names)->SV {
    const auto index=static_cast<std::size_t>(value);require(index<N,Error::schema);return names[index];
}
template<class E,std::size_t N> auto enumeration(const Json& value,const std::array<SV,N>& names)->E {
    const auto& name=text(value);const auto found=std::ranges::find(names,name);require(found!=names.end(),Error::schema);
    return static_cast<E>(found-names.begin());
}
auto check_ref(const pf::CheckRef& value)->Json {
    return core::detail::json_object<Json>([&](Json& row){row["kind"]=token(value.kind,check_names);
        row["scope"]=std::to_string(value.scope);row["item"]=std::to_string(value.item);});
}
auto check_ref(const Json& value)->pf::CheckRef {
    fields(value,{"kind","scope","item"});
    return {enumeration<pf::CheckKind>(value.at("kind"),check_names),decimal(value.at("scope")),decimal(value.at("item"))};
}
auto optional_number(std::optional<std::uint64_t> value)->Json {return value?Json(std::to_string(*value)):Json(nullptr);}
auto preflight_tree(const pf::detail::State& state,Work& work)->Json {
    Json result=Json::object();core::detail::JsonCleanup cleanup{result};
    const auto& refs=state.references;auto& references=result["references"];
    references["controller"]=std::to_string(refs.controller);references["normalization"]=std::to_string(refs.normalization);
    references["system_state"]=std::to_string(refs.system_state);references["base_build"]=std::to_string(refs.base_build);
    result["started_at"]=state.started_at;result["ended_at"]=state.ended_at;
    for(const auto key:{"scopes","workers","caches","nodes","checks","temperatures","overrides","warnings","failures"})result[key]=Json::array();
    result["monitoring"]=state.monitoring?Json(state.monitoring->canonical()):Json(nullptr);result["status"]=token(state.status,status_names);
    for(const auto& row:state.scopes){work.debit(1U);core::detail::append_object_json(result["scopes"],[&](Json& value){
        value["matrix"]=row.matrix;value["experiment_index"]=std::to_string(row.experiment_index);value["baseline_index"]=std::to_string(row.baseline_index);
        value["candidate_index"]=std::to_string(row.candidate_index);value["experiment_id"]=hex(row.experiment_id,work);
        value["baseline_id"]=hex(row.baseline_id,work);value["candidate_id"]=hex(row.candidate_id,work);});}
    for(const auto& row:state.workers){work.debit(1U);core::detail::append_object_json(result["workers"],[&](Json& value){
        value["scope"]=std::to_string(row.scope);value["role"]=std::to_string(row.role);value["index"]=std::to_string(row.index);
        value["stable_pu"]=hex(row.stable_pu,work);value["stable_core"]=hex(row.stable_core,work);value["linux_cpu"]=std::to_string(row.linux_cpu.value());
        value["working_bytes"]=std::to_string(row.working_bytes);value["page_bytes"]=std::to_string(row.page_bytes);
        value["threshold_numerator"]=std::to_string(row.threshold_numerator);value["threshold_denominator"]=std::to_string(row.threshold_denominator);
        value["prefault"]=row.prefault;value["verify_before"]=row.verify_before;value["verify_after"]=row.verify_after;});}
    for(const auto& row:state.caches){work.debit(1U);core::detail::append_object_json(result["caches"],[&](Json& value){
        value["worker"]=std::to_string(row.worker);value["cache"]=std::to_string(row.cache.value());value["level"]=std::to_string(row.level);
        value["kind"]=token(row.kind,cache_names);value["bytes"]=std::to_string(row.bytes);value["line_bytes"]=std::to_string(row.line_bytes);});}
    for(const auto& row:state.nodes){work.debit(1U);core::detail::append_object_json(result["nodes"],[&](Json& value){
        value["worker"]=std::to_string(row.worker);value["stable_id"]=hex(row.stable_id,work);value["linux_node"]=std::to_string(row.linux_node.value());
        value["local"]=row.local?Json(*row.local):Json(nullptr);});}
    for(const auto& row:state.checks){work.debit(1U);core::detail::append_object_json(result["checks"],[&](Json& value){
        core::detail::set_json(value,"ref",check_ref(row.ref));value["required"]=row.required;value["evaluation"]=token(row.evaluation,evaluation_names);
        value["outcome"]=row.outcome?Json(token(*row.outcome,outcome_names)):Json(nullptr);value["reason"]=token(row.reason,reason_names);
        value["checked_at"]=row.checked_at?Json(*row.checked_at):Json(nullptr);value["failure_event"]=optional_number(row.failure_event);
        value["requirement_token"]=hex(row.requirement_token,work);});}
    for(const auto& row:state.temperatures){work.debit(1U);if(row.value)require(row.value->sensor_kind()==monitoring::TemperatureSensorKind::cpu_control,Error::schema);
        core::detail::append_object_json(result["temperatures"],[&](Json& value){value["state"]=token(row.state,thermal_names);
            value["sensor_kind"]=row.value?Json("cpu_control"):Json(nullptr);value["milli_celsius"]=row.value?Json(std::to_string(row.value->temperature().value())):Json(nullptr);
            value["observed_at"]=row.observed_at?Json(*row.observed_at):Json(nullptr);value["reason"]=row.reason?Json(token(*row.reason,thermal_reason_names)):Json(nullptr);
            value["failure_event"]=optional_number(row.failure_event);});}
    for(const auto& row:state.overrides){work.debit(1U);core::detail::append_object_json(result["overrides"],[&](Json& value){
        core::detail::set_json(value,"check",check_ref(row.check));value["attempted_at"]=row.attempted_at;value["allowed"]=row.allowed;});}
    for(const auto& row:state.warnings){work.debit(1U);core::detail::append_json(result["warnings"],check_ref(row));}
    for(const auto& row:state.failures){work.debit(1U);core::detail::append_object_json(result["failures"],[&](Json& value){
        core::detail::set_json(value,"check",check_ref(row.check));value["event"]=std::to_string(row.event);});}
    return Json(std::move(result));
}
auto timestamp(const Json& value)->journal::Timestamp {
    auto parsed=journal::Timestamp::parse(text(value));core::detail::ExpectedErrorCleanup cleanup{parsed};
    require(parsed.has_value(),Error::schema);return std::move(*parsed);
}
auto nullable_number(const Json& value)->std::optional<std::uint64_t> {
    if(value.is_null())return {};
    return decimal(value);
}
auto monitoring_fact(SV bytes,const pf::detail::FactualContext& context,const PreflightLimits& bound,Work& work)->validation_event::Owner {
    auto value=parse(bytes,bound,work);core::detail::JsonCleanup value_cleanup{value};
    fields(value,{"schema","run_id","assessment_ordinal","producer","policy","checks","capability_status","warning_check_ids"});
    require(value.at("checks").is_array()&&value.at("checks").size()==4U,Error::schema);
    auto projected=runtime::detail::project_scenario_monitoring_inputs(context.effective);core::detail::ExpectedErrorCleanup projected_cleanup{projected};
    require(projected.has_value()&&projected->has_value(),Error::association);const auto& p=**projected;
    auto policy=runtime::detail::project_production_monitoring_policy(runtime::detail::ProductionMonitoringPolicyView{
        p.required,p.optional,p.before,p.after,p.during,p.interval,p.unavailable});core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    require(policy.has_value(),Error::association);
    auto run=journal::RunId::parse(context.run);core::detail::ExpectedErrorCleanup run_cleanup{run};require(run.has_value(),Error::identity);
    std::array<std::optional<journal::Timestamp>,4> times;
    std::array<validation_event::Slot,4> slots{};
    constexpr std::array<SV,3> evaluations{"not_requested","not_evaluated","evaluated"};
    for(std::size_t i{};i<4U;++i){const auto& row=value.at("checks")[i];
        fields(row,{"id","code","authority","channel","requested","evaluation","support","checked_at",
            "outcome","absence_reason","preparation_effect","effective_channel"});
        slots[i].evaluation=enumeration<validation_event::Evaluation>(row.at("evaluation"),evaluations);
        if(!row.at("support").is_null()){
            const auto& support=text(row.at("support"));require(support=="supported"||support=="unsupported",Error::schema);
            slots[i].support=support=="supported"?monitoring::MonitoringSourceSupport::supported:monitoring::MonitoringSourceSupport::unsupported;
        }
        if(!row.at("checked_at").is_null()){times[i].emplace(timestamp(row.at("checked_at")));slots[i].checked_at=&*times[i];}
    }
    auto retained=validation_event::retain({*run,decimal(value.at("assessment_ordinal")),validation_event::Producer::controller,*policy,slots},bound.monitoring);
    core::detail::ExpectedErrorCleanup retained_cleanup{retained};
    require(retained.has_value()&&work.equal(retained->canonical(),bytes),Error::association);return std::move(*retained);
}
auto preflight_allowance(const PreflightLimits& bound,Work& work)->std::size_t {
    require(bound.depth&&bound.depth<=64U,Error::capacity);
    const Shape tree{bound.nodes,bound.nodes,bound.text,bound.depth,bound.bytes};
    const auto rows=add(add(bound.evaluator.checks,bound.evaluator.temperatures),add(bound.evaluator.overrides,bound.evaluator.failures));
    const auto declarations=multiply(rows,add(sizeof(pf::TemperatureFact),add(sizeof(pf::OverrideAttempt),
        add(sizeof(pf::CheckDeclaration),add(sizeof(pf::FailureReference),sizeof(journal::Timestamp))))));
    const auto peak=add(multiply(workspace(tree),4U),add(declarations,
        add(bound.evaluator.consumer_simultaneous_bytes,add(bound.monitoring.retained_bytes,bound.monitoring.canonical_bytes))));
    require(peak<=bound.simultaneous,Error::capacity);work.reserve_peak(peak);
    work.debit(add(rows,add(bound.evaluator.consumer_simultaneous_bytes,bound.monitoring.canonical_bytes)));return peak;
}
} // namespace

auto preflight_context(const normalization_serialization::Owner& normalization,const host_serialization::Owner& host,
    const base_build_serialization::Owner& base)->pf::detail::FactualContext {
    const auto* plan_owner=normalization.plan();const auto* topology=host.topology();
    require(normalization.provenance()&&host.provenance()&&base.provenance()&&plan_owner&&topology&&topology->topology(),Error::child);
    require(normalization.provenance()->run==host.provenance()->run&&host.provenance()->run==base.provenance()->run&&
        normalization.provenance()->domain==host.provenance()->domain&&host.provenance()->domain==base.provenance()->domain,Error::association);
    const auto* ancestry=plan_owner->document_ancestry();const auto* kernel=plan_owner->kernel_dispatch();
    const auto* comparison=plan_owner->comparison_experiments();const auto* matrix=plan_owner->matrix_experiments();
    const auto* resource=plan_owner->resource_service();
    require(ancestry&&ancestry->default_expanded()&&kernel&&kernel->workloads()&&kernel->experiment_workloads()&&
        comparison&&comparison->experiments()&&matrix&&matrix->experiments()&&resource&&resource->matrix_worker_templates()&&
        resource->experiment_workers()&&resource->reserved_service_cores()&&base.disposition(),Error::child);
    auto slot=[]<class Child,class Value>(const Child* child,const Value* value)->pf::detail::SlotView<Value>{
        require(child&&child->slot_state(),Error::child);pf::detail::SlotState state{};
        switch(*child->slot_state()){
        case topology_serialization::SlotState::not_attempted:state=pf::detail::SlotState::not_attempted;break;
        case topology_serialization::SlotState::unavailable:state=pf::detail::SlotState::unavailable;break;
        case topology_serialization::SlotState::failed:state=pf::detail::SlotState::failed;break;
        case topology_serialization::SlotState::retained:state=pf::detail::SlotState::retained;break;
        }
        std::optional<std::uint64_t> failure;
        if(const auto* error=child->failure_error()){require(error->provenance(),Error::child);failure=error->provenance()->sequence;}
        return {state,value,failure};
    };
    require(host.system_controls()&&host.kernel_identity()&&host.pu_identity()&&host.frequency()&&host.anonymous_thp()&&host.process_thp(),Error::child);
    pf::detail::HostFacts facts{slot(host.system_controls(),host.system_controls()->controls()),
        slot(host.kernel_identity(),host.kernel_identity()->kernel_identity()),slot(host.pu_identity(),host.pu_identity()->identity()),
        slot(host.frequency(),host.frequency()->source()),slot(host.anonymous_thp(),host.anonymous_thp()->source()),
        slot(host.process_thp(),host.process_thp()->source())};
    return {normalization.provenance()->run,*topology->topology(),*ancestry->default_expanded(),*kernel->workloads(),
        *kernel->experiment_workloads(),*comparison->experiments(),*matrix->experiments(),*resource->matrix_worker_templates(),
        *resource->experiment_workers(),*resource->reserved_service_cores(),facts,base.manifest(),
        *base.disposition()!=process_subject::BuildState::not_attempted};
}
auto encode_preflight(const pf::detail::State& state,const PreflightLimits& bound,Work& work)->std::string {
    (void)preflight_allowance(bound,work);require(pf::detail::fits(state.usage,bound.evaluator),Error::capacity);
    require(state.scopes.size()<=bound.evaluator.scopes&&state.workers.size()<=bound.evaluator.worker_associations&&
        state.caches.size()<=bound.evaluator.cache_memberships&&state.nodes.size()<=bound.evaluator.target_nodes&&
        state.checks.size()<=bound.evaluator.checks&&state.temperatures.size()<=bound.evaluator.temperatures&&
        state.overrides.size()<=bound.evaluator.overrides&&state.warnings.size()<=bound.evaluator.warnings&&
        state.failures.size()<=bound.evaluator.failures,Error::capacity);
    std::size_t strings=add(state.started_at.size(),state.ended_at.size());
    for(const auto& row:state.scopes)strings=add(strings,multiply(add(row.experiment_id.size(),add(row.baseline_id.size(),row.candidate_id.size())),2U));
    for(const auto& row:state.workers)strings=add(strings,multiply(add(row.stable_pu.size(),row.stable_core.size()),2U));
    for(const auto& row:state.nodes)strings=add(strings,multiply(row.stable_id.size(),2U));
    for(const auto& row:state.checks)strings=add(strings,add(multiply(row.requirement_token.size(),2U),row.checked_at?row.checked_at->size():0U));
    for(const auto& row:state.temperatures)strings=add(strings,row.observed_at?row.observed_at->size():0U);
    for(const auto& row:state.overrides)strings=add(strings,row.attempted_at.size());
    if(state.monitoring)strings=add(strings,state.monitoring->canonical().size());
    require(strings<=bound.text,Error::capacity);work.debit(strings);
    auto value=preflight_tree(state,work);core::detail::JsonCleanup value_cleanup{value};auto canonical=core::canonicalize_json_jcs(value,{bound.bytes,bound.depth,bound.nodes});
    core::detail::ExpectedErrorCleanup cleanup{canonical};require(canonical.has_value(),Error::canonical);return std::move(*canonical);
}
auto decode_preflight(SV bytes,const pf::detail::FactualContext& context,const PreflightLimits& bound,Work& work)
    ->std::unique_ptr<pf::detail::State> {
    (void)preflight_allowance(bound,work);auto value=parse(bytes,bound,work);core::detail::JsonCleanup value_cleanup{value};
    fields(value,{"references","started_at","ended_at","scopes","workers","caches","nodes","checks","temperatures","overrides","warnings","failures","monitoring","status"});
    for(const auto key:{"checks","temperatures","overrides","failures"})require(value.at(key).is_array(),Error::schema);
    const auto checks=value.at("checks").size(),temperatures=value.at("temperatures").size(),overrides=value.at("overrides").size(),failures=value.at("failures").size();
    require(checks<=bound.evaluator.checks&&temperatures<=bound.evaluator.temperatures&&overrides<=bound.evaluator.overrides&&failures<=bound.evaluator.failures,Error::capacity);
    std::vector<journal::Timestamp> times;times.reserve(add(add(checks,temperatures),add(overrides,2U)));
    auto time_pointer=[&](const Json& incoming)->const journal::Timestamp*{
        if(incoming.is_null())return nullptr;
        times.push_back(timestamp(incoming));return &times.back();};
    const auto* start=time_pointer(value.at("started_at"));const auto* end=time_pointer(value.at("ended_at"));require(start&&end,Error::schema);
    std::vector<pf::CheckDeclaration> declarations;declarations.reserve(checks);
    for(const auto& row:value.at("checks")){
        fields(row,{"ref","required","evaluation","outcome","reason","checked_at","failure_event","requirement_token"});
        declarations.push_back({check_ref(row.at("ref")),enumeration<pf::Evaluation>(row.at("evaluation"),evaluation_names),
            time_pointer(row.at("checked_at"))});
    }
    std::vector<pf::TemperatureFact> thermal;thermal.reserve(temperatures);
    for(const auto& row:value.at("temperatures")){
        fields(row,{"state","sensor_kind","milli_celsius","observed_at","reason","failure_event"});
        pf::TemperatureFact fact;fact.state=enumeration<pf::ThermalState>(row.at("state"),thermal_names);
        if(!row.at("milli_celsius").is_null()){
            const auto& raw=text(row.at("milli_celsius"));std::int64_t signed_value{};
            const auto parsed=std::from_chars(raw.data(),raw.data()+raw.size(),signed_value);
            require(parsed.ec==std::errc{}&&parsed.ptr==raw.data()+raw.size()&&std::to_string(signed_value)==raw&&text(row.at("sensor_kind"))=="cpu_control",Error::schema);
            fact.value=monitoring::TemperatureSnapshot::create(monitoring::TemperatureSensorKind::cpu_control,
                monitoring::TemperatureMilliCelsius::from_millicelsius(signed_value));
        }else require(row.at("sensor_kind").is_null(),Error::schema);
        fact.observed_at=time_pointer(row.at("observed_at"));fact.failure_event=nullable_number(row.at("failure_event"));
        if(!row.at("reason").is_null())fact.reason=enumeration<pf::ThermalReason>(row.at("reason"),thermal_reason_names);
        thermal.push_back(std::move(fact));
    }
    std::vector<pf::OverrideAttempt> attempts;attempts.reserve(overrides);
    for(const auto& row:value.at("overrides")){fields(row,{"check","attempted_at","allowed"});const auto* time=time_pointer(row.at("attempted_at"));require(time,Error::schema);attempts.push_back({check_ref(row.at("check")),*time});}
    std::vector<pf::FailureReference> failed;failed.reserve(failures);
    for(const auto& row:value.at("failures")){fields(row,{"check","event"});failed.push_back({check_ref(row.at("check")),decimal(row.at("event"))});}
    const auto& refs=value.at("references");fields(refs,{"controller","normalization","system_state","base_build"});
    pf::Input input{{decimal(refs.at("controller")),decimal(refs.at("normalization")),decimal(refs.at("system_state")),decimal(refs.at("base_build"))},
        *start,*end,declarations,{},thermal,failed,attempts};
    if(!value.at("monitoring").is_null())input.monitoring.emplace(monitoring_fact(text(value.at("monitoring")),context,bound,work));
    auto evaluated=pf::detail::evaluate(context,input,bound.evaluator,0U,0U);require(evaluated.has_value(),Error::association);
    if(input.monitoring)(*evaluated)->monitoring.emplace(std::move(*input.monitoring));
    auto expected=preflight_tree(**evaluated,work);core::detail::JsonCleanup expected_cleanup{expected};same(value,expected,bound,work);
    require((*evaluated)->usage.retained_bytes<=bound.retained,Error::capacity);return std::move(*evaluated);
}
} // namespace epycbench::results::event_profile::detail
