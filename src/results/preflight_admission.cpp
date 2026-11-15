#include "results/preflight_admission.hpp"
#include "runtime/scenario_monitoring_policy_projection.hpp"
#include "runtime/production_admission_policy.hpp"
#include "epycbench/topology/serialization.hpp"
#include "core/json_ownership.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <utility>

namespace epycbench::results::preflight {
namespace {
constexpr std::array dimensions{
    &Quantities::records, &Quantities::scopes, &Quantities::checks,
    &Quantities::worker_associations, &Quantities::cache_memberships, &Quantities::target_nodes,
    &Quantities::requirement_tokens, &Quantities::temperatures, &Quantities::timestamps,
    &Quantities::warnings, &Quantities::overrides, &Quantities::failures,
    &Quantities::text_bytes, &Quantities::canonical_child_bytes, &Quantities::retained_bytes,
    &Quantities::consumer_simultaneous_bytes};
void require(bool condition, Error error = Error::invalid_input) { if (!condition) throw error; }
void add(std::size_t& value, std::size_t amount, std::size_t width = 1U) {
    require(width == 0U || amount <= (SIZE_MAX - value) / width, Error::capacity);
    value += amount * width;
}
auto source_sequence(const std::optional<std::string>& text) -> std::optional<std::uint64_t> {
    if (!text) return {};
    const auto pos=text->rfind(':');
    require(pos!=std::string::npos,Error::reference);
    std::uint64_t value{};
    auto parsed=std::from_chars(text->data()+pos+1U,text->data()+text->size(),value);
    require(parsed.ec==std::errc{} && parsed.ptr==text->data()+text->size(),Error::reference);
    return value;
}
struct Fact {
    std::optional<Outcome> outcome{Outcome::pass};
    Reason reason{Reason::none};
    std::optional<std::uint64_t> failure;
};
auto unknown(Reason reason=Reason::unknown_fact) -> Fact { return {Outcome::unavailable,reason,{}}; }
template<class Observation> auto observed(const Observation& value) -> Fact {
    using S=decltype(value.state);
    if(value.state==S::not_attempted)return {{},Reason::not_attempted,{}};
    if(value.state==S::unavailable)return unknown(Reason::source_unavailable);
    if(value.state==S::failed)return {Outcome::fail,Reason::source_failed,source_sequence(value.failure_event)};
    return {};
}
template<class T> auto attribute(const topology::Attribute<T>& value) -> Fact {
    if(value.value())return {};
    return unknown(value.availability()==topology::Availability::conflicted?
        Reason::conflicting_fact:Reason::unknown_fact);
}
template<class T> auto missing(const detail::SlotView<T>& slot) -> Fact {
    if(slot.state==detail::SlotState::not_attempted)return {{},Reason::not_attempted,{}};
    if(slot.state==detail::SlotState::failed)return {Outcome::fail,Reason::source_failed,slot.failure};
    return unknown(Reason::source_unavailable);
}
template<class T> auto borrowed(const host_assembly::Slot<T>& slot) noexcept -> detail::SlotView<T> {
    if (const auto* value=std::get_if<T>(&slot))return {detail::SlotState::retained,value,{}};
    if (const auto* failure=std::get_if<host_assembly::Failed>(&slot))
        return {detail::SlotState::failed,nullptr,failure->failure_event_sequence};
    return {std::holds_alternative<host_assembly::NotAttempted>(slot)?
        detail::SlotState::not_attempted:detail::SlotState::unavailable,nullptr,{}};
}
template<class T> auto valid_slot(const detail::SlotView<T>& slot) noexcept -> bool {
    switch(slot.state) {
    case detail::SlotState::not_attempted: case detail::SlotState::unavailable:
        return !slot.value && !slot.failure;
    case detail::SlotState::failed: return !slot.value && slot.failure.has_value();
    case detail::SlotState::retained: return slot.value && !slot.failure;
    }
    return false;
}
struct Recorder {
    detail::State& state;
    const Input& input;
    const Limits& limits;
    std::size_t prior, session_stock, input_stock;
    DeclarationInventory* declaration_inventory{};
    void budget() {
        auto peak=prior;
        add(peak,session_stock); add(peak,input_stock); add(peak,detail::fixed_stock());
        add(peak,state.usage.retained_bytes);
        // The incoming assessment is included only once, even before its transfer.
        state.usage.consumer_simultaneous_bytes=peak;
        require(detail::fits(state.usage,limits),Error::capacity);
    }
    void stock(std::size_t& count,std::size_t amount,std::size_t width) {
        add(count,amount);
        require(width==0U || count<=static_cast<std::size_t>(PTRDIFF_MAX)/width,Error::capacity);
        add(state.usage.retained_bytes,amount,width);budget();
    }
    void text(std::string_view value) {
        require(value.size()<=std::string{}.max_size(),Error::capacity);
        add(state.usage.text_bytes,value.size());add(state.usage.retained_bytes,value.size());budget();
    }
    void time(std::string_view value) {
        require(value.size()==30U);add(state.usage.timestamps,1U);text(value);
    }
    void row(CheckRef ref,bool required,Fact fact={},std::string_view token={},bool locked=false,
             std::optional<std::string_view> exact_time={},bool time_locked=false) {
        Evaluation evaluation{};
        std::optional<std::string_view> checked_at;
        if(declaration_inventory) {
            require(state.usage.checks<limits.checks,Error::capacity);
            evaluation=locked && fact.outcome ? Evaluation::evaluated : Evaluation::not_evaluated;
            checked_at=exact_time;
            add(state.usage.retained_bytes,sizeof(DeclarationRequirement));budget();
            if(exact_time)text(*exact_time);
            declaration_inventory->rows.push_back({ref,required,
                locked?std::optional{evaluation}:std::nullopt,time_locked,
                exact_time?std::optional<std::string>{*exact_time}:std::nullopt});
        } else {
            const auto position=state.usage.checks;
            require(position<input.declarations.size());
            const auto& declaration=input.declarations[position];
            require(declaration.check==ref && (declaration.evaluation==Evaluation::not_evaluated ||
                declaration.evaluation==Evaluation::evaluated));
            evaluation=declaration.evaluation;
            if(declaration.checked_at)checked_at=declaration.checked_at->text();
            require(!checked_at || checked_at->size()==30U);
            require(evaluation==Evaluation::evaluated || !checked_at);
            if(locked)require((evaluation==Evaluation::evaluated)==fact.outcome.has_value());
            if(time_locked)require(checked_at==exact_time);
        }
        if(evaluation==Evaluation::not_evaluated)fact.outcome.reset();
        // An attempted declaration cannot turn an unattempted source into a pass.
        const bool reject=fact.outcome==Outcome::fail ||
            (required && fact.outcome==Outcome::unavailable) || fact.failure.has_value();
        const bool incomplete=required && !fact.outcome;
        const bool warning=fact.outcome==Outcome::warning || (!required && fact.outcome==Outcome::unavailable);
        if(reject)state.status=Status::rejected;
        else if(incomplete && state.status!=Status::rejected)state.status=Status::incomplete;
        else if(warning && state.status==Status::accepted)state.status=Status::accepted_with_warnings;
        stock(state.usage.checks,1U,sizeof(Check));
        if(!token.empty()){add(state.usage.requirement_tokens,1U);text(token);}
        if(checked_at)time(*checked_at);
        if(warning){stock(state.usage.warnings,1U,sizeof(CheckRef));state.warnings.push_back(ref);}
        state.checks.push_back({ref,required,evaluation,fact.outcome,fact.reason,
            checked_at?std::optional<std::string>{*checked_at}:std::nullopt,
            fact.failure,std::string{token}});
    }
};
auto effective(const topology::TopologySnapshot& graph,core::LinuxCpuId cpu) -> bool {
    const auto& set=graph.machine().effective_cpus.value();return set && set->contains(cpu);
}
void inventory(Recorder& r,const detail::FactualContext& context,const topology::LoadedTopology& loaded) {
    const auto& graph=loaded.topology;
    const auto& machine=graph.machine();
    const auto& categories=context.host;
    r.row({CheckKind::capture_model},true,attribute(machine.cpu_model));
    const auto* identity=categories.pu_identity.value;
    for(const auto& pu:graph.processing_units())if(effective(graph,pu.linux_cpu_id)) {
        Fact fact=missing(categories.pu_identity);
        if(identity) {
            auto found=std::ranges::find(identity->rows(),pu.linux_cpu_id,&pu_cpu_identity::OwnedRow::linux_cpu);
            fact=found==identity->rows().end()?unknown():observed(found->signature);
        }
        r.row({CheckKind::capture_signature,0U,pu.linux_cpu_id.value()},true,fact);
    }
    r.row({CheckKind::capture_online},true);
    r.row({CheckKind::capture_numa},true,attribute(machine.effective_nodes));
    r.row({CheckKind::capture_cores},true,attribute(machine.effective_cpus));
    r.row({CheckKind::capture_caches},true,graph.caches().empty()?unknown():Fact{});
    for(const auto& node:graph.numa_nodes())if(node.linux_node_id.value() && machine.effective_nodes.value() &&
        machine.effective_nodes.value()->contains(core::NumaNodeId{*node.linux_node_id.value()}))
        r.row({CheckKind::capture_memory,0U,*node.linux_node_id.value()},true,attribute(node.installed_memory));
    const auto* kernel=categories.kernel_identity.value;
    for(std::size_t i=0;i<3U;++i)r.row({CheckKind::capture_kernel,0U,i},true,
        kernel?observed(kernel->observations()[i]):missing(categories.kernel_identity));
    r.row({CheckKind::capture_build},true,context.build?Fact{}:
        context.build_attempted?unknown(Reason::source_unavailable):Fact{{},Reason::not_attempted,{}});
    r.row({CheckKind::capture_compiler},true,context.build?Fact{}:
        context.build_attempted?unknown(Reason::source_unavailable):Fact{{},Reason::not_attempted,{}});
    const auto* controls=categories.system_controls.value;
    for(std::size_t i=0;i<5U;++i)r.row({CheckKind::capture_controls,0U,i},true,
        controls?observed(controls->observations()[i]):missing(categories.system_controls));
    const auto* thp=categories.anonymous_thp.value;
    Fact enumeration=missing(categories.anonymous_thp);
    if(thp)enumeration=observed(*thp->enumeration());
    r.row({CheckKind::capture_anonymous_thp},true,enumeration);
    if(thp)for(std::size_t i=0;i<thp->rows().size();++i)
        r.row({CheckKind::capture_anonymous_thp,0U,i+1U},true,observed(thp->rows()[i]));
    const auto* process=categories.process_thp.value;
    r.row({CheckKind::capture_process_thp},true,process?observed(*process->observation()):missing(categories.process_thp));
    r.row({CheckKind::capture_scenario},true);
    Fact numa=controls?observed(controls->observations()[2]):missing(categories.system_controls);
    if(numa.outcome==Outcome::pass && std::get<std::uint32_t>(controls->observations()[2].value)!=0U)
        numa.outcome=Outcome::warning;
    r.row({CheckKind::numa_balancing},false,numa);
}
void requirements(Recorder& r,const detail::FactualContext& context,const topology::TopologySnapshot& graph) {
    const auto& root=context.effective;
    const auto* requirements=root.contains("requirements")?&root.at("requirements"):nullptr;
    const auto get=[&](std::string_view parent,std::string_view child={}) -> const nlohmann::ordered_json* {
        if(!requirements || !requirements->contains(parent))return nullptr;
        const auto* value=&requirements->at(parent);
        return child.empty()?value:value->contains(child)?&value->at(child):nullptr;
    };
    const Fact na{Outcome::not_applicable,Reason::none,{}};
    const auto* os=get("operating_system");
    Fact os_fact=na;
    if(os) {
        const auto& slot=context.host.kernel_identity;
        const auto* kernel=slot.value;
        os_fact=kernel?observed(kernel->observations()[0]):missing(slot);
        if(os_fact.outcome==Outcome::pass)os_fact.outcome=
            *os=="linux" && kernel->observations()[0].reported_octets=="Linux"?Outcome::pass:Outcome::fail;
    }
    r.row({CheckKind::operating_system},os!=nullptr,os_fact);
    const auto any=[&](CheckKind kind,const nlohmann::ordered_json* values,std::optional<std::string_view> actual) {
        Fact result=na;
        if(values){result=actual?Fact{Outcome::fail,Reason::none,{}}:unknown();
            for(const auto& value:*values)if(actual && value.get_ref<const std::string&>()==*actual)result.outcome=Outcome::pass;}
        r.row({kind},values!=nullptr,result);
    };
    any(CheckKind::architecture,get("architecture","any_of"),graph.machine().architecture);
    any(CheckKind::cpu_vendor,get("cpu_vendor","any_of"),graph.machine().cpu_vendor.value()?
        std::optional<std::string_view>{*graph.machine().cpu_vendor.value()}:std::nullopt);
    const auto minimum=[&](CheckKind kind,const nlohmann::ordered_json* value,bool memory) {
        Fact result=na;std::size_t count{};bool known=graph.machine().effective_nodes.value().has_value() &&
            (memory || graph.machine().effective_cpus.value().has_value());
        if(value) {
            for(const auto& node:graph.numa_nodes())if(node.linux_node_id.value() && graph.machine().effective_nodes.value() &&
                graph.machine().effective_nodes.value()->contains(core::NumaNodeId{*node.linux_node_id.value()})) {
                if(memory){if(!node.installed_memory.value())known=false;else if(node.installed_memory.value()->value()>0U)++count;}
                else {
                    bool found=false;
                    for(const auto& pu:graph.processing_units()) {
                        if(node.member_pus.contains(pu.id) && effective(graph,pu.linux_cpu_id))found=true;
                    }
                    if(found)++count;
                }
            }
            const auto requested=value->get<std::uint64_t>();
            result=count>=requested?Fact{}:known?Fact{Outcome::fail,Reason::none,{}}:unknown();
        }
        r.row({kind},value!=nullptr,result);
    };
    minimum(CheckKind::cpu_nodes,get("numa","minimum_cpu_nodes"),false);
    minimum(CheckKind::memory_nodes,get("numa","minimum_memory_nodes"),true);
    const auto* smt=get("smt","permitted");Fact smt_fact=na;
    if(smt){smt_fact=Fact{};if(!smt->get<bool>()){
        smt_fact=attribute(graph.machine().effective_cpus);
        for(const auto& a:graph.processing_units())if(effective(graph,a.linux_cpu_id))
            for(const auto& b:graph.processing_units())if(a.id!=b.id && a.core_id==b.core_id && effective(graph,b.linux_cpu_id))
                smt_fact={Outcome::fail,Reason::none,{}};
    }}
    r.row({CheckKind::smt_permitted},smt!=nullptr,smt_fact);
    for(const auto [name,kind]:std::array{std::pair{"required",CheckKind::required_feature},
                                       std::pair{"preferred",CheckKind::preferred_feature}}) {
        const auto* features=get("features",name);
        if(features)for(std::size_t i=0;i<features->size();++i)
            r.row({kind,0U,i},kind==CheckKind::required_feature,unknown(Reason::unsupported_rule),
                (*features)[i].get_ref<const std::string&>());
    }
}
void scopes(Recorder& r,const detail::FactualContext& context,const topology::LoadedTopology& loaded) {
    const auto& graph=loaded.topology;
    const auto* frequency=context.host.frequency.value;
    const auto worker=[&](std::size_t scope,std::size_t role,std::size_t index,
        const scenario::ResolvedProcessingUnit& selected,const auto& placement,std::uint64_t bytes) {
        auto pu=std::ranges::find(graph.processing_units(),selected.linux_cpu,&topology::ProcessingUnit::linux_cpu_id);
        require(pu!=graph.processing_units().end() && effective(graph,selected.linux_cpu),Error::reference);
        const auto worker_index=r.state.workers.size();
        r.stock(r.state.usage.worker_associations,1U,sizeof(Worker));
        r.text(selected.stable_id.value());r.text(selected.core_id.value());
        r.state.workers.push_back({scope,role,index,std::string{selected.stable_id.value()},
            std::string{selected.core_id.value()},selected.linux_cpu,bytes,placement.page_size.value(),
            placement.placement_threshold.numerator(),placement.placement_threshold.denominator(),
            placement.prefault,placement.verify_before,placement.verify_after});
        for(const auto& cache:graph.caches())if(cache.member_pus.contains(pu->id)) {
            r.stock(r.state.usage.cache_memberships,1U,sizeof(CacheMembership));
            r.state.caches.push_back({worker_index,cache.id,cache.level,cache.kind,cache.size.value(),cache.line_size.value()});
        }
        const auto target=[&](const scenario::ResolvedMemoryNode& node) {
            const auto actual=std::ranges::find_if(graph.numa_nodes(),[&](const auto& n){
                return n.linux_node_id.value() && *n.linux_node_id.value()==node.linux_node.value();});
            require(actual!=graph.numa_nodes().end(),Error::reference);
            r.stock(r.state.usage.target_nodes,1U,sizeof(TargetNode));r.text(node.stable_id.value());
            r.state.nodes.push_back({worker_index,std::string{node.stable_id.value()},node.linux_node,
                actual->member_pus.contains(pu->id)});
        };
        if constexpr(std::same_as<std::remove_cvref_t<decltype(placement)>,scenario::ResolvedPerWorkerMemoryPlacement>) {
            require(index<placement.worker_nodes.size() && placement.worker_nodes[index].worker==selected,Error::reference);
            target(placement.worker_nodes[index].node);
        } else for(const auto& node:placement.nodes)target(node);
    };
    const auto edge=[&](bool matrix,std::size_t experiment,std::string_view experiment_id,
        std::size_t baseline_index,std::size_t candidate_index,std::string_view baseline_id,
        std::string_view candidate_id,const auto& left_workers,const auto& right_workers,
        const auto& left,const auto& right,const kernel::KernelConfiguration& configuration) {
        r.stock(r.state.usage.scopes,1U,sizeof(Scope));
        r.text(experiment_id);r.text(baseline_id);r.text(candidate_id);
        r.state.scopes.push_back({matrix,experiment,baseline_index,candidate_index,
            std::string{experiment_id},std::string{baseline_id},std::string{candidate_id}});
        const auto scope=r.state.scopes.size();const auto begin=r.state.workers.size();
        const auto bytes=std::visit([](const auto& config){
            if constexpr(requires{config.working_set_per_worker();})return config.working_set_per_worker().value();
            else return config.working_set().value();},configuration);
        const auto role=[&](std::size_t role_index,const auto& selected,const auto& placement){
            const auto add_workers=[&](const auto& value){for(std::size_t i=0;i<selected.size();++i)
                worker(scope,role_index,i,selected[i],value,bytes);};
            if constexpr(std::same_as<std::remove_cvref_t<decltype(placement)>,scenario::ComparisonMemoryPlacement>)
                std::visit(add_workers,placement);
            else add_workers(placement);
        };
        role(0U,left_workers,left);role(1U,right_workers,right);
        const frequency_policy_owner::Policy* first{};bool different=false,uncovered=false,shared=false,overlap=false;
        bool local[2]{true,true},remote[2]{true,true},verified=true;
        for(std::size_t i=begin;i<r.state.workers.size();++i) {
            const auto& w=r.state.workers[i];
            for(std::size_t j=begin;j<i;++j)if(r.state.workers[j].role==w.role && r.state.workers[j].stable_core==w.stable_core)shared=true;
            for(const auto& reserved:context.reserved_cores)if(reserved.value()==w.stable_core)overlap=true;
            verified&=w.verify_before && w.verify_after;
            bool has_node=false;
            for(const auto& node:r.state.nodes)if(node.worker==i){has_node=true;local[w.role]&=node.local==true;remote[w.role]&=node.local==false;}
            if(!has_node){local[w.role]=false;remote[w.role]=false;}
            const frequency_policy_owner::Policy* policy{};
            if(frequency)for(const auto& p:frequency->policies())for(const auto& member:p.related_cpus)
                if(member.linux_cpu==w.linux_cpu)policy=&p;
            Fact frequency_fact=missing(context.host.frequency);
            if(policy){frequency_fact={};if(first)different|=first->scaling_driver!=policy->scaling_driver ||
                first->scaling_governor!=policy->scaling_governor || first->scaling_minimum!=policy->scaling_minimum ||
                first->scaling_maximum!=policy->scaling_maximum;else first=policy;
            } else {
                uncovered=true;
                if(frequency){frequency_fact=unknown();for(const auto& absent:frequency->uncovered_cpus())
                    if(absent.association.linux_cpu==w.linux_cpu)frequency_fact=observed(absent);}
            }
            r.row({CheckKind::capture_frequency,scope,i-begin},true,frequency_fact);
        }
        r.row({CheckKind::cpu_exclusivity,scope},false,unknown(Reason::unsupported_rule));
        std::optional<bool> intended;
        if(matrix){for(const auto& spec:context.worker_templates)if(spec.experiment_index==experiment)
            intended=spec.smt!=scenario::SelectorSmtMode::one_per_core;
        }else{
            const auto& root=context.effective;
            for(const auto& binding:context.worker_bindings)if(binding.experiment_index==experiment){
                const auto& set=root.at("resources").at("worker_sets").at(binding.worker_set_name);
                if(set.contains("select") && set.at("select").contains("smt")){
                    const auto& smt=set.at("select").at("smt");
                    if(smt.is_string())intended=smt!="one-per-core";
                    else if(smt.is_object())intended=true;
                }
            }
        }
        Fact sharing=shared?(intended?Fact{*intended?Outcome::pass:Outcome::warning,Reason::none,{}}:unknown()):Fact{};
        r.row({CheckKind::unintended_smt,scope},false,sharing);
        r.row({CheckKind::hierarchy,scope},false,unknown(Reason::unsupported_rule));
        r.row({CheckKind::frequency_difference,scope},false,uncovered?unknown(Reason::source_unavailable):
            Fact{different?Outcome::warning:Outcome::pass,Reason::none,{}});
        const bool controlled=(local[0] && remote[1]) || (remote[0] && local[1]);
        r.row({CheckKind::placement_intent,scope},controlled,
            {controlled && !verified?Outcome::fail:Outcome::not_applicable,Reason::pending_preparation,{}});
        r.row({CheckKind::service_core,scope},true,{overlap?Outcome::fail:Outcome::pass,Reason::none,{}});
    };
    for(const auto& experiment:context.comparisons) {
        const auto workload=context.workloads.find(experiment.workload_id);
        require(workload!=context.workloads.end(),Error::reference);
        const auto baseline=std::ranges::find(experiment.variants,experiment.baseline_variant_id,&scenario::ResolvedComparisonVariant::variant_id);
        require(baseline!=experiment.variants.end(),Error::reference);
        for(const auto& candidate_id:experiment.candidate_variant_ids){
            const auto candidate=std::ranges::find(experiment.variants,candidate_id,&scenario::ResolvedComparisonVariant::variant_id);
            require(candidate!=experiment.variants.end(),Error::reference);
            edge(false,experiment.experiment_index,experiment.experiment_id,baseline->variant_index,candidate->variant_index,
                baseline->variant_id,candidate->variant_id,experiment.workers,experiment.workers,
                baseline->memory_placement,candidate->memory_placement,workload->second.configuration);
        }
    }
    for(const auto& experiment:context.matrices){
        const auto binding=std::ranges::find(context.workload_bindings,experiment.experiment_index,&scenario::ExperimentWorkloadBinding::experiment_index);
        require(binding!=context.workload_bindings.end(),Error::reference);
        const auto workload=context.workloads.find(binding->workload_id);
        require(workload!=context.workloads.end(),Error::reference);
        for(const auto& pair:experiment.comparison_edges){
            require(pair.baseline_cell_index<experiment.cells.size() && pair.candidate_cell_index<experiment.cells.size(),Error::reference);
            const auto& baseline=experiment.cells[pair.baseline_cell_index];const auto& candidate=experiment.cells[pair.candidate_cell_index];
            require(baseline.worker_row_index<experiment.worker_rows.size() && candidate.worker_row_index<experiment.worker_rows.size(),Error::reference);
            edge(true,experiment.experiment_index,experiment.experiment_id,pair.baseline_cell_index,pair.candidate_cell_index,
                baseline.memory_node_stable_id.value(),candidate.memory_node_stable_id.value(),
                experiment.worker_rows[baseline.worker_row_index].workers,experiment.worker_rows[candidate.worker_row_index].workers,
                baseline.memory_placement,candidate.memory_placement,workload->second.configuration);
        }
    }
}
void monitoring_checks(Recorder& r,const detail::FactualContext& context) {
    const auto& root=context.effective;
    std::size_t channels{};
    if(root.contains("monitoring")){
        add(channels,root.at("monitoring").at("required").size());
        add(channels,root.at("monitoring").at("optional").size());require(channels<=4U,Error::policy);
    }
    auto projected=runtime::detail::project_scenario_monitoring_inputs(root);
    core::detail::ExpectedErrorCleanup projected_cleanup{projected};
    require(projected.has_value(),Error::policy);
    if(!*projected){
        require(!r.input.monitoring,Error::reference);
        r.row({CheckKind::monitoring_policy},true,{{},Reason::missing_policy,{}});
        for(std::size_t i=0;i<4U;++i)r.row({CheckKind::monitoring_channel,0U,i},false,{{},Reason::missing_policy,{}});
        return;
    }
    const auto& value=**projected;
    require(std::ranges::is_sorted(value.required) && std::ranges::is_sorted(value.optional) &&
        std::ranges::adjacent_find(value.required)==value.required.end() &&
        std::ranges::adjacent_find(value.optional)==value.optional.end(),Error::policy);
    for(const auto channel:value.required)
        require(std::ranges::find(value.optional,channel)==value.optional.end(),Error::policy);
    auto policy=runtime::detail::project_production_monitoring_policy(runtime::detail::ProductionMonitoringPolicyView{
        value.required,value.optional,value.before,value.after,value.during,value.interval,value.unavailable});
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if(!policy){
        require(!r.input.monitoring,Error::reference);
        require(policy.error().code.value()=="runtime.runner_state.admission_failed" &&
            policy.error().context.value("underlying_code",std::string{})=="monitoring.driver.invalid_policy",Error::policy);
        r.row({CheckKind::monitoring_policy},true,{Outcome::fail,Reason::policy_failure,{}});
        for(std::size_t i=0;i<4U;++i)r.row({CheckKind::monitoring_channel,0U,i},false,{{},Reason::policy_failure,{}});
        return;
    }
    r.row({CheckKind::monitoring_policy},true);
    if(r.input.monitoring)require(r.input.monitoring->run_id()==context.run &&
        r.input.monitoring->producer()==validation_event::Producer::controller && r.input.monitoring->ordinal() &&
        r.input.monitoring->policy() && *r.input.monitoring->policy()==*policy,Error::reference);
    for(std::size_t i=0;i<4U;++i){
        const auto disposition=policy->disposition(static_cast<monitoring::MonitoringChannel>(i));
        const bool required=disposition==monitoring::MonitoringChannelDisposition::required ||
            (disposition==monitoring::MonitoringChannelDisposition::optional &&
             policy->optional_unsupported()==monitoring::OptionalUnavailableSourcePolicy::fail);
        if(r.input.monitoring){
            const auto& check=r.input.monitoring->checks()[i];
            r.row({CheckKind::monitoring_channel,0U,i},required,{check.outcome,
                check.outcome?Reason::none:Reason::not_attempted,{}},{},true,
                check.checked_at?std::optional<std::string_view>{*check.checked_at}:std::nullopt,true);
        }else r.row({CheckKind::monitoring_channel,0U,i},required,
            disposition==monitoring::MonitoringChannelDisposition::disabled?
                Fact{Outcome::not_applicable,Reason::none,{}}:Fact{{},Reason::not_attempted,{}});
    }
}
void temperatures(Recorder& r) {
    for(std::size_t i=0;i<r.input.temperatures.size();++i){
        const auto& t=r.input.temperatures[i];
        require(t.state>=ThermalState::not_attempted && t.state<=ThermalState::failed);
        require(!t.reason || (*t.reason>=ThermalReason::not_supported && *t.reason<=ThermalReason::unsupported_value));
        require(!t.observed_at || t.observed_at->text().size()==30U);
        Fact fact;
        switch(t.state){
        case ThermalState::not_attempted:
            require(!t.value && !t.observed_at && !t.reason && !t.failure_event);
            fact={{},Reason::not_attempted,{}};break;
        case ThermalState::available:
            require(t.value && t.observed_at && !t.reason && !t.failure_event &&
                t.value->sensor_kind()==monitoring::TemperatureSensorKind::cpu_control);break;
        case ThermalState::unavailable:
            require(!t.value && t.reason && *t.reason<=ThermalReason::access_denied && !t.failure_event);
            fact=unknown(Reason::source_unavailable);break;
        case ThermalState::failed:
            require(!t.value && t.observed_at && t.reason && *t.reason>=ThermalReason::read_failed && t.failure_event);
            fact={Outcome::fail,Reason::source_failed,t.failure_event};break;
        }
        r.row({CheckKind::thermal_observation,0U,i},false,fact,{},true,
            t.observed_at?std::optional<std::string_view>{t.observed_at->text()}:std::nullopt,true);
        r.stock(r.state.usage.temperatures,1U,sizeof(Temperature));
        if(t.observed_at)r.time(t.observed_at->text());
        r.state.temperatures.push_back({t.state,t.value,t.observed_at?
            std::optional<std::string>{t.observed_at->text()}:std::nullopt,t.reason,t.failure_event});
    }
    r.row({CheckKind::thermal_suitability},false,unknown(Reason::unsupported_rule));
}
void overrides(Recorder& r) {
    for(const auto& supplied:r.input.overrides){
        require(supplied.attempted_at.text().size()==30U);
        const auto row=std::ranges::find(r.state.checks,supplied.check,&Check::ref);
        require(row!=r.state.checks.end(),Error::reference);
        require(std::ranges::find(r.state.overrides,supplied.check,&Override::check)==r.state.overrides.end(),Error::reference);
        const bool allowed=!row->required && (row->outcome==Outcome::warning || row->outcome==Outcome::unavailable) &&
            row->ref.kind!=CheckKind::placement_intent && !row->failure_event;
        r.stock(r.state.usage.overrides,1U,sizeof(Override));r.time(supplied.attempted_at.text());
        r.state.overrides.push_back({supplied.check,supplied.attempted_at.text(),allowed});
    }
}
auto factual_context(const detail::Context& context) -> detail::FactualContext {
    const auto& expanded=context.source.plan()->matrix_expanded();
    const auto& dispatched=expanded.kernel_dispatched();
    const auto& service=dispatched.memory_placement_resolved().service_cpu_resolved();
    const auto& resource=service.resource_resolved();
    const auto& host=*context.host.categories();
    return {context.run,context.topology,resource.default_expanded().value(),
        dispatched.workloads(),dispatched.experiment_workloads(),expanded.comparison_experiments(),
        expanded.matrix_experiments(),resource.matrix_worker_templates(),resource.experiment_workers(),
        service.reserved_service_cores(),{borrowed(host.system_controls),borrowed(host.kernel_identity),
            borrowed(host.pu_identity),borrowed(host.frequency),borrowed(host.anonymous_thp),borrowed(host.process_thp)},
        context.build,context.build_attempted};
}
auto evaluate_impl(const detail::FactualContext&,const Input&,const Limits&,
    std::size_t,std::size_t,DeclarationInventory*) -> std::expected<std::unique_ptr<detail::State>,Error>;
struct Staging {
    const detail::FactualContext* context;
    const Input* input;
    const Limits* limits;
    Usage initial, returned;
    std::size_t prior, session_stock, input_stock, channels;
    std::unique_ptr<detail::State> candidate;
    std::optional<std::expected<topology::LoadedTopology,core::Error>> loaded, loaded_return;
    std::optional<std::expected<std::optional<runtime::ipc::detail::MonitoringValues>,core::Error>> parsed, parsed_return;
    runtime::ipc::detail::MonitoringValues parser_local;
    std::optional<runtime::ipc::detail::MonitoringValues> parser_optional_return;
    runtime::detail::ProductionMonitoringPolicyView view, view_parameter;
    std::array<monitoring::MonitoringChannelDisposition,4> factory_dispositions;
    bool factory_before, factory_after, factory_during;
    std::optional<core::Duration> factory_interval;
    monitoring::OptionalUnavailableSourcePolicy factory_optional;
    std::optional<std::expected<monitoring::MonitoringExecutionPolicy,core::Error>> policy, policy_return, policy_factory, policy_wrapper_parameter;
    std::optional<std::expected<Owner,Error>> result, result_return;
    Recorder recorder;
    Fact fact, fact_parameter;
    // Conservative named logical cells, not a compiler stack-layout claim.
    std::size_t declaration_position, worker_index, cpu_node_count, memory_node_count,
        experiment_index, candidate_index, scope_index, role_index, selected_index,
        cache_index, target_index, temperature_index, failure_index, override_index;
    bool reject, incomplete, warning, minimum_known, minimum_found, sharing,
        explicit_intent, policy_required, override_allowed;
    CheckRef row_reference;
    std::string_view row_token;
    std::optional<std::string> row_timestamp;
    std::string row_owned_token;
    Check row_construction;
    Scope scope_construction;
    Worker worker_construction;
    CacheMembership cache_construction;
    TargetNode target_construction;
    Temperature temperature_construction;
    Override override_construction;
    detail::Context context_temporary;
    detail::FactualContext factual_context;
    std::expected<std::unique_ptr<detail::State>,Error> factual_result, factual_return;
    std::array<detail::SlotView<host_controls::Owner>,6> slot_return_cells;
    const void *expanded, *dispatched, *service, *resource, *host,
        *borrowed_value, *borrowed_failure;
    Owner owner_construction;
    std::unique_ptr<detail::State> owner_parameter;
    core::detail::ExpectedErrorCleanup<std::expected<topology::LoadedTopology,core::Error>> loaded_cleanup;
    core::detail::ExpectedErrorCleanup<std::expected<std::optional<runtime::ipc::detail::MonitoringValues>,core::Error>> parsed_cleanup;
    core::detail::ExpectedErrorCleanup<std::expected<monitoring::MonitoringExecutionPolicy,core::Error>> policy_cleanup;
    nlohmann::json parser_error_context, factory_error_context, wrapper_error_context;
    core::detail::JsonCleanup<nlohmann::json> parser_context_cleanup, factory_context_cleanup, wrapper_context_cleanup;
    nlohmann::json classified_value, component_value, underlying_code_value;
    std::string parser_error_code, parser_classification, factory_error_code, factory_classification, wrapper_classification;
    core::detail::ExpectedErrorCleanup<std::expected<monitoring::MonitoringExecutionPolicy,core::Error>> wrapper_cleanup;
    std::optional<core::Error> error;
};
} // namespace

auto detail::fits(const Usage& usage,const Limits& limits) noexcept -> bool {
    for(auto dimension:dimensions)if(usage.*dimension>limits.*dimension)return false;
    return true;
}
auto detail::fixed_stock() noexcept -> std::size_t {
    // Named consumer cells only; Topology loader dynamics use its accepted structural limits.
    return sizeof(Staging)+4U*sizeof(runtime::ipc::PlacementMonitoringChannel);
}
Owner::Owner(std::unique_ptr<detail::State> state) noexcept:state_(std::move(state)){}
Owner::~Owner()=default;
Owner::Owner(Owner&&) noexcept=default;
auto Owner::operator=(Owner&&) noexcept -> Owner& = default;
auto Owner::usage() const noexcept -> Usage{return state_?state_->usage:Usage{};}
auto Owner::status() const noexcept -> std::optional<Status>{return state_?std::optional{state_->status}:std::nullopt;}
void Owner::commit(std::optional<validation_event::Owner>& assessment) noexcept {state_->monitoring=std::move(assessment);}
auto Owner::prepare(const detail::Context& context,const Input& input,const Limits& limits,
    std::size_t prior,std::size_t session_stock) -> std::expected<Owner,Error> {
    if (!context.source.plan() || !context.host.categories())return std::unexpected{Error::reference};
    const auto facts=factual_context(context);
    auto result=detail::evaluate(facts,input,limits,prior,session_stock);
    if (!result)return std::unexpected{result.error()};
    return Owner{std::move(*result)};
}
namespace {
auto evaluate_impl(const detail::FactualContext& context,const Input& input,const Limits& limits,
    std::size_t prior,std::size_t session_stock,DeclarationInventory* declaration_inventory)
    -> std::expected<std::unique_ptr<detail::State>,Error> {
    try{
        require(limits.records<=1U,Error::capacity);
        require(context.topology.digest() && context.effective.is_object() &&
            (!context.build || context.build_attempted),Error::reference);
        require(valid_slot(context.host.system_controls) && valid_slot(context.host.kernel_identity) &&
            valid_slot(context.host.pu_identity) && valid_slot(context.host.frequency) &&
            valid_slot(context.host.anonymous_thp) && valid_slot(context.host.process_thp),Error::reference);
        require(input.started_at.text().size()==30U && input.ended_at.text().size()==30U);
        Usage initial{};initial.records=1U;initial.retained_bytes=sizeof(Owner)+sizeof(detail::State);
        if(input.monitoring){
            require(input.monitoring->policy() && input.monitoring->usage().retained_bytes>=sizeof(validation_event::Owner),Error::reference);
            initial.canonical_child_bytes=input.monitoring->usage().canonical_bytes;
            add(initial.retained_bytes,input.monitoring->usage().retained_bytes-sizeof(validation_event::Owner));
        }
        require(input.declarations.size()<=limits.checks && input.temperatures.size()<=limits.temperatures &&
            input.overrides.size()<=limits.overrides && input.failures.size()<=limits.failures,Error::capacity);
        std::size_t input_stock=sizeof(Input);
        if(declaration_inventory)add(input_stock,sizeof(DeclarationInventory));
        add(input_stock,input.declarations.size(),sizeof(CheckDeclaration));
        add(input_stock,input.temperatures.size(),sizeof(TemperatureFact));
        add(input_stock,input.overrides.size(),sizeof(OverrideAttempt));
        add(input_stock,input.failures.size(),sizeof(FailureReference));
        initial.consumer_simultaneous_bytes=prior;
        add(initial.consumer_simultaneous_bytes,session_stock);add(initial.consumer_simultaneous_bytes,input_stock);
        add(initial.consumer_simultaneous_bytes,detail::fixed_stock());add(initial.consumer_simultaneous_bytes,initial.retained_bytes);
        require(detail::fits(initial,limits),Error::capacity);
        auto state=std::make_unique<detail::State>();state->usage=initial;state->references=input.references;
        Recorder recorder{*state,input,limits,prior,session_stock,input_stock,declaration_inventory};
        recorder.time(input.started_at.text());recorder.time(input.ended_at.text());
        state->started_at=input.started_at.text();state->ended_at=input.ended_at.text();
        {
            auto loaded=topology::deserialize_topology(context.topology.canonical(),topology::TopologyJsonInputPolicy::require_canonical);
            core::detail::ExpectedErrorCleanup cleanup{loaded};
            require(loaded.has_value(),Error::topology);
            require(context.topology.fingerprints() && loaded->fingerprints==*context.topology.fingerprints(),Error::reference);
            inventory(recorder,context,*loaded);
            requirements(recorder,context,loaded->topology);
            scopes(recorder,context,*loaded);
        }
        monitoring_checks(recorder,context);temperatures(recorder);
        if(!declaration_inventory)require(state->checks.size()==input.declarations.size());
        for(const auto& reference:input.failures){
            require(std::ranges::find(state->failures,reference.check,&FailureReference::check)==state->failures.end(),Error::reference);
            const auto found=std::ranges::find(state->checks,reference.check,&Check::ref);
            require(found!=state->checks.end() && found->failure_event==reference.event,Error::reference);
            recorder.stock(state->usage.failures,1U,sizeof(FailureReference));state->failures.push_back(reference);
        }
        for(const auto& row:state->checks)if(row.ref.kind==CheckKind::thermal_observation && row.failure_event)
            require(std::ranges::find(state->failures,row.ref,&FailureReference::check)!=state->failures.end(),Error::reference);
        overrides(recorder);recorder.budget();
        return state;
    }catch(const Error error){return std::unexpected{error};}
    catch(const std::bad_alloc&){throw;}
    catch(...){return std::unexpected{Error::invalid_input};}
}
} // namespace
auto detail::evaluate(const FactualContext& context,const Input& input,const Limits& limits,
    std::size_t prior,std::size_t session_stock) -> std::expected<std::unique_ptr<State>,Error> {
    return evaluate_impl(context,input,limits,prior,session_stock,nullptr);
}
auto detail::declarations(const Context& context,const Input& input,const Limits& limits,
    std::size_t prior,std::size_t session_stock) -> std::expected<DeclarationInventory,Error> {
    if(!context.source.plan() || !context.host.categories())return std::unexpected{Error::reference};
    if(!input.declarations.empty())return std::unexpected{Error::invalid_input};
    DeclarationInventory result;
    auto evaluated=evaluate_impl(factual_context(context),input,limits,prior,session_stock,&result);
    if(!evaluated)return std::unexpected{evaluated.error()};
    result.usage=(*evaluated)->usage;
    return result;
}
} // namespace epycbench::results::preflight
