#include "results/execution_request_association.hpp"
#include "results/process_subject_admission.hpp"
#include <limits>

namespace epycbench::results::request_association {
namespace {
using Z=std::size_t;
using U=std::uint64_t;
using J=nlohmann::ordered_json;
using SV=std::string_view;
namespace rt=runtime;
namespace ipc=runtime::ipc;
namespace sc=scenario;
namespace ep=execution_plan;
using E=Error;
struct Work {
    Usage used;
    const Limits* limits;
    void add(Z& counter,Z amount,Z ceiling) {
        if (amount>ceiling || counter>ceiling-amount) throw E::capacity;
        counter+=amount;
    }
    void debit(Z amount=1) { add(used.work,amount,limits->work); }
    auto equal(SV first,SV second)->bool {
        const auto bytes=detail::checked_add(first.size(),second.size());
        if(!bytes)throw E::capacity;
        const auto charge=detail::checked_add(*bytes,1);
        if(!charge)throw E::capacity;
        add(used.compared_bytes,*bytes,limits->compared_bytes);debit(*charge);
        return first==second;
    }
};
void check(bool predicate,E error) { if(!predicate)throw error; }
auto member(const J& object,SV key,Work& work,E error,bool required=true)->const J* {
    work.debit();check(object.is_object(),error);
    auto current=object.cbegin();const auto end=object.cend();
    for(;current!=end;++current) {
        work.debit();if(work.equal(current.key(),key))return &current.value();
    }
    work.debit();check(!required,error);return nullptr;
}
auto number(const J& value,Work& work,E error)->U {
    work.debit();check(value.is_number_unsigned(),error);return value.get_ref<const J::number_unsigned_t&>();
}
auto boolean(const J& value,Work& work,E error)->bool {
    work.debit();check(value.is_boolean(),error);return value.get_ref<const J::boolean_t&>();
}
auto string(const J& value,Work& work,E error)->SV {
    work.debit();check(value.is_string(),error);return value.get_ref<const J::string_t&>();
}
void ratio(const J& value,const sc::ExactRatio& actual,Work& work,E error) {
    const auto* numerator=member(value,"numerator",work,error);
    const auto* denominator=member(value,"denominator",work,error);
    work.debit();check(number(*numerator,work,error)==actual.numerator(),error);
    work.debit();check(number(*denominator,work,error)==actual.denominator(),error);
}
struct Roots {
    const build_object_association::Owner* build{};
    const implementation_selection::Owner* selection{};
    const normalization_serialization::Owner* normalization{};
    const private_plan_composition::Owner* plan{};
    const document_ancestry_serialization::Owner* document{};
    const resource_service_serialization::Owner* resource{};
    const kernel_dispatch_serialization::Owner* kernel{};
    const comparison_experiments_serialization::Owner* comparisons{};
    const matrix_experiments_serialization::Owner* matrices{};
    const J* normalized{};const J* effective{};const J* measurement{};const J* monitoring{};
    const ep::Pair* pair{};
    std::span<const implementation_selection::Workload> selected;
    std::span<const implementation_selection::Binding> bindings;
    const topology::TopologySnapshot* topology{};
    const topology::StableTopologyIds* stable_ids{};
    const topology::TopologyFingerprints* fingerprints{};
};
auto roots(const ep::Owner& owner,U ordinal,Work& work)->Roots {
    Roots result;
    result.build=owner.evidence();work.debit();check(result.build,E::state);
    result.selection=result.build->selection();work.debit();check(result.selection,E::state);
    result.normalization=result.selection->normalization();work.debit();check(result.normalization,E::state);
    result.plan=result.normalization->plan();work.debit();check(result.plan,E::state);
    result.document=result.plan->document_ancestry();work.debit();check(result.document,E::state);
    result.resource=result.plan->resource_service();work.debit();check(result.resource,E::state);
    result.kernel=result.plan->kernel_dispatch();work.debit();check(result.kernel,E::state);
    result.comparisons=result.plan->comparison_experiments();work.debit();check(result.comparisons,E::state);
    result.matrices=result.plan->matrix_experiments();work.debit();check(result.matrices,E::state);
    result.normalized=result.document->unit_normalized();work.debit();check(result.normalized,E::state);
    result.effective=result.document->default_expanded();work.debit();check(result.effective,E::state);
    result.measurement=member(*result.effective,"measurement",work,E::state);
    result.monitoring=member(*result.effective,"monitoring",work,E::state,false);
    const auto pairs=owner.pairs();work.debit();check(ordinal<pairs.size(),E::reference);
    result.pair=&pairs[static_cast<Z>(ordinal)];work.debit();check(result.pair->ordinal==ordinal,E::reference);
    result.selected=result.selection->workloads();result.bindings=result.selection->bindings();
    return result;
}
struct Row {
    const sc::ResolvedComparisonExperiment* ordinary{};
    const sc::ExpandedMatrixExperiment* matrix{};
    const sc::ResolvedMatrixCell* cell{};
    const sc::DispatchedWorkload* workload{};
    const sc::ExperimentWorkloadBinding* binding{};
    std::span<const sc::ResolvedProcessingUnit> workers;
    const sc::ComparisonMemoryPlacement* memory{};
    const sc::MatrixComparisonConfiguration* comparison{};
    SV experiment;
};
auto row(const Roots& root,Z side,Work& work)->Row {
    Row result;
    const auto& pair=*root.pair;
    const auto selected=root.selected;
    const auto bindings=root.bindings;
    work.debit();check(pair.experiment_index<bindings.size() && pair.workload_index<selected.size(),E::reference);
    work.debit();check(bindings[pair.experiment_index].experiment_index==pair.experiment_index &&
        bindings[pair.experiment_index].workload_index==pair.workload_index,E::reference);
    const auto* workloads=root.kernel->workloads();
    const auto* dispatched=root.kernel->experiment_workloads();
    work.debit();check(workloads && dispatched && pair.experiment_index<dispatched->size(),E::state);
    result.binding=&(*dispatched)[pair.experiment_index];
    work.debit();check(result.binding->experiment_index==pair.experiment_index,E::reference);
    work.debit();check(work.equal(result.binding->workload_id,selected[pair.workload_index].workload_id),E::identity);
    auto current=workloads->cbegin();const auto end=workloads->cend();
    for(;current!=end;++current) {
        work.debit();
        if(work.equal(current->first,result.binding->workload_id)) {result.workload=&current->second;break;}
    }
    work.debit();check(result.workload,E::reference);
    work.debit();check(result.workload->kernel_id==selected[pair.workload_index].semantic,E::identity);
    const Z index=side==0?pair.baseline_index:pair.candidate_index;
    if(pair.kind==ep::PairKind::comparison) {
        const auto* experiments=root.comparisons->experiments();
        work.debit();check(experiments && pair.child_experiment_index<experiments->size(),E::reference);
        result.ordinary=&(*experiments)[pair.child_experiment_index];
        work.debit();check(result.ordinary->experiment_index==pair.experiment_index,E::reference);
        work.debit();check(index<result.ordinary->variants.size(),E::reference);
        work.debit();check(result.ordinary->variants[index].variant_index==index,E::reference);
        work.debit();check(work.equal(result.ordinary->workload_id,result.binding->workload_id),E::identity);
        result.workers=result.ordinary->workers;result.memory=&result.ordinary->variants[index].memory_placement;
        result.comparison=&result.ordinary->comparison;result.experiment=result.ordinary->experiment_id;
    } else {
        work.debit();check(pair.kind==ep::PairKind::matrix,E::reference);
        const auto* experiments=root.matrices->experiments();
        work.debit();check(experiments && pair.child_experiment_index<experiments->size(),E::reference);
        result.matrix=&(*experiments)[pair.child_experiment_index];
        work.debit();check(result.matrix->experiment_index==pair.experiment_index && index<result.matrix->cells.size(),E::reference);
        result.cell=&result.matrix->cells[index];
        work.debit();check(result.cell->experiment_index==pair.experiment_index &&
            result.cell->worker_row_index<result.matrix->worker_rows.size(),E::reference);
        result.workers=result.matrix->worker_rows[result.cell->worker_row_index].workers;
        result.comparison=&result.matrix->comparison;result.experiment=result.matrix->experiment_id;
    }
    return result;
}
void worker(const ipc::PlacementCpuAssignment& actual,const sc::ResolvedProcessingUnit& expected,Work& work) {
    work.debit();check(work.equal(actual.stable_pu_id().value(),expected.stable_id.value()),E::placement);
    work.debit();check(work.equal(actual.stable_core_id().value(),expected.core_id.value()),E::placement);
    work.debit();check(actual.linux_cpu()==expected.linux_cpu,E::placement);
}
void node(const ipc::PlacementNumaTarget& actual,const sc::ResolvedMemoryNode& expected,Work& work) {
    work.add(work.used.node_targets,1,work.limits->node_targets);work.debit();
    work.debit();check(work.equal(actual.stable_numa_node_id().value(),expected.stable_id.value()),E::placement);
    work.debit();check(actual.linux_numa_node()==expected.linux_node,E::placement);
}
void region(const ipc::PlacementRegionRequest& actual,sc::MemoryPolicy policy,sc::PageMode page,
            core::ByteCount size,const sc::ExactRatio& threshold,bool prefault,bool before,bool after,Work& work) {
    work.debit();check(policy==sc::MemoryPolicy::bind && actual.policy()==ipc::PlacementPolicy::bind,E::placement);
    work.debit();check(page==sc::PageMode::base && actual.page_mode()==ipc::PlacementPageMode::base,E::placement);
    work.debit();check(actual.page_size_bytes()==size,E::placement);
    work.debit();check(actual.placement_threshold().numerator()==threshold.numerator(),E::placement);
    work.debit();check(actual.placement_threshold().denominator()==threshold.denominator(),E::placement);
    work.debit();check(actual.prefault()==prefault,E::placement);work.debit();check(actual.verify_before()==before,E::placement);
    work.debit();check(actual.verify_after()==after,E::placement);
}
void uniform(const ipc::PlacementRegionRequest& actual,const sc::ResolvedMemoryPlacement& expected,Work& work) {
    region(actual,expected.policy,expected.page_mode,expected.page_size,expected.placement_threshold,
        expected.prefault,expected.verify_before,expected.verify_after,work);
    const auto targets=actual.nodes();work.debit();check(targets.size()==expected.nodes.size(),E::placement);
    for(Z index=0;index<targets.size();++index) {work.debit();node(targets[index],expected.nodes[index],work);}
}
void placement(const ipc::PlacementWorkerRequest& actual,const Row& expected,Z index,Work& work) {
    const auto& memory=actual.region();
    if(expected.cell) {uniform(memory,expected.cell->memory_placement,work);return;}
    work.debit();check(expected.memory,E::reference);
    if(const auto* value=std::get_if<sc::ResolvedMemoryPlacement>(expected.memory)) {uniform(memory,*value,work);return;}
    const auto* value=std::get_if<sc::ResolvedPerWorkerMemoryPlacement>(expected.memory);
    work.debit();check(value && index<value->worker_nodes.size(),E::reference);
    const auto& mapping=value->worker_nodes[index];
    worker(actual.cpu(),mapping.worker,work);
    region(memory,value->policy,value->page_mode,value->page_size,value->placement_threshold,
        value->prefault,value->verify_before,value->verify_after,work);
    const auto targets=memory.nodes();work.debit();check(targets.size()==1,E::placement);node(targets[0],mapping.node,work);
}
auto kernel(const rt::AdmittedKernelExecutionInputs& actual,const Row& expected,Work& work)->core::ByteCount {
    if(const auto* bw=std::get_if<kernel::SequentialReadBandwidthConfig>(&expected.workload->configuration)) {
        const auto* value=std::get_if<ipc::PlacementBandwidthExecutionInputs>(&actual);
        work.debit();check(value,E::placement);work.debit();check(value->configuration()==*bw,E::placement);
        work.debit();check(!expected.binding->pointer_cycle_seed && !expected.binding->pointer_cycle_seed_origin,E::placement);
        return bw->working_set_per_worker();
    }
    const auto* latency=std::get_if<kernel::DependentLoadLatencyConfig>(&expected.workload->configuration);
    const auto* value=std::get_if<ipc::PlacementLatencyExecutionInputs>(&actual);
    work.debit();check(latency && value && expected.binding->pointer_cycle_seed && expected.binding->pointer_cycle_seed_origin,E::placement);
    work.debit();check(value->configuration()==*latency,E::placement);
    work.debit();check(value->resolved_permutation_seed()==*expected.binding->pointer_cycle_seed,E::placement);
    work.debit();check(value->seed_origin()==*expected.binding->pointer_cycle_seed_origin,E::placement);
    return latency->working_set();
}
void measurement(const rt::AdmittedMeasurementInputs& actual,const Roots& root,U seed,Work& work) {
    const auto* observation=member(*root.measurement,"observation",work,E::measurement);
    const auto* calibration=member(*root.measurement,"calibration",work,E::measurement);
    const auto* warmup=member(*root.measurement,"warmup",work,E::measurement);
    const auto* repetition=member(*root.measurement,"repetition",work,E::measurement);
    const auto* stopping=member(*root.measurement,"stopping",work,E::measurement);
    work.debit();check(actual.observation_target_duration().value()==number(*member(*member(*observation,"target_duration",work,E::measurement),"value",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.observation_minimum_duration().value()==number(*member(*member(*observation,"minimum_duration",work,E::measurement),"value",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.observation_maximum_duration().value()==number(*member(*member(*observation,"maximum_duration",work,E::measurement),"value",work,E::measurement),work,E::measurement),E::measurement);
    ratio(*member(*observation,"maximum_harness_overhead",work,E::measurement),actual.maximum_harness_overhead(),work,E::measurement);
    work.debit();check(actual.calibration_enabled()==boolean(*member(*calibration,"enabled",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.calibration_strategy()==ipc::PlacementCalibrationStrategy::geometric_refine,E::measurement);
    work.debit();check(work.equal(string(*member(*calibration,"strategy",work,E::measurement),work,E::measurement),"geometric-refine"),E::measurement);
    work.debit();check(actual.warmup_minimum()==number(*member(*warmup,"minimum",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.warmup_maximum()==number(*member(*warmup,"maximum",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.warmup_stability_window()==number(*member(*warmup,"stability_window",work,E::measurement),work,E::measurement),E::measurement);
    ratio(*member(*warmup,"stability_threshold",work,E::measurement),actual.warmup_stability_threshold(),work,E::measurement);
    work.debit();check(actual.repetition_minimum_blocks()==number(*member(*repetition,"minimum_blocks",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.repetition_maximum_blocks()==number(*member(*repetition,"maximum_blocks",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.minimum_valid_observations_per_variant()==number(*member(*repetition,"minimum_valid_observations_per_variant",work,E::measurement),work,E::measurement),E::measurement);
    work.debit();check(actual.stopping_enabled()==boolean(*member(*stopping,"enabled",work,E::measurement),work,E::measurement),E::measurement);
    ratio(*member(*stopping,"confidence",work,E::measurement),actual.stopping_confidence(),work,E::measurement);
    ratio(*member(*stopping,"ratio_interval_relative_half_width",work,E::measurement),actual.stopping_precision(),work,E::measurement);
    work.debit();check(actual.comparison_order()==root.pair->order,E::measurement);work.debit();check(actual.root_seed()==seed,E::measurement);
}
void statistics(const detail::RequestView& inputs,const Roots& root,const Row& selected,Work& work) {
    const auto& source=inputs.statistics_source;const auto& projected=inputs.statistics;
    work.debit();check(source.experiment_kind()==(root.pair->kind==ep::PairKind::comparison?rt::StatisticsExperimentKind::comparison:rt::StatisticsExperimentKind::matrix_comparison),E::statistics);
    work.debit();check(source.comparison_confidence()==selected.comparison->confidence,E::statistics);
    const auto* statistics=member(*root.measurement,"statistics",work,E::statistics);
    work.debit();check(source.bootstrap_samples()==number(*member(*statistics,"bootstrap_samples",work,E::statistics),work,E::statistics),E::statistics);
    ratio(*member(*statistics,"practical_effect",work,E::statistics),source.global_practical_effect(),work,E::statistics);
    const auto* experiments=member(*root.normalized,"experiments",work,E::statistics);
    work.debit();check(experiments->is_array() && root.pair->experiment_index<experiments->size(),E::statistics);
    work.debit();const auto* experiment=&(*experiments)[root.pair->experiment_index];
    const auto* container=member(*experiment,root.pair->kind==ep::PairKind::comparison?"compare":"comparison",work,E::statistics);
    const auto* local=member(*container,"practical_effect",work,E::statistics,false);
    const auto& actual_local=source.local_practical_effect();
    work.debit();check((local!=nullptr)==actual_local.has_value(),E::statistics);
    if(local)ratio(*local,*actual_local,work,E::statistics);
    work.debit();check(source.effective_practical_effect()==selected.comparison->practical_effect,E::statistics);
    const auto& comparison=projected.comparison_bootstrap_policy();
    const auto& stopping=projected.stopping_bootstrap_policy();
    work.debit();check(comparison.replicate_count().value()==source.bootstrap_samples(),E::statistics);
    work.debit();check(stopping.replicate_count().value()==source.bootstrap_samples(),E::statistics);
    const auto first_seed=comparison.seed();const auto second_seed=stopping.seed();
    work.debit();check(first_seed==second_seed,E::statistics);
}
auto channel(SV text,Work& work)->Z {
    if(work.equal(text,"page_faults"))return 0;
    if(work.equal(text,"context_switches"))return 1;
    if(work.equal(text,"temperature"))return 2;
    if(work.equal(text,"frequency"))return 3;
    throw E::monitoring;
}
void channels(const J& source,std::span<const ipc::PlacementMonitoringChannel> actual,
              monitoring::MonitoringChannelDisposition disposition,
              std::array<monitoring::MonitoringChannelDisposition,4>& expected,Work& work) {
    work.debit();check(source.is_array() && source.size()==actual.size(),E::monitoring);
    for(Z index=0;index<actual.size();++index) {
        work.debit();const Z target=channel(string(source[index],work,E::monitoring),work);
        work.debit();check(actual[index]==static_cast<ipc::PlacementMonitoringChannel>(target),E::monitoring);
        work.debit();check(expected[target]==monitoring::MonitoringChannelDisposition::disabled,E::monitoring);
        expected[target]=disposition;
    }
}
void monitoring_policy(const ipc::PlacementMonitoringInputs& actual,const monitoring::MonitoringExecutionPolicy& policy,
                       const Roots& root,Work& work) {
    work.debit();check(root.monitoring,E::monitoring);
    std::array<monitoring::MonitoringChannelDisposition,4> expected{};
    channels(*member(*root.monitoring,"required",work,E::monitoring),actual.required(),monitoring::MonitoringChannelDisposition::required,expected,work);
    channels(*member(*root.monitoring,"optional",work,E::monitoring),actual.optional(),monitoring::MonitoringChannelDisposition::optional,expected,work);
    for(Z index=0;index<expected.size();++index) {
        work.debit();work.debit();check(policy.disposition(static_cast<monitoring::MonitoringChannel>(index))==expected[index],E::monitoring);
    }
    const auto* sampling=member(*root.monitoring,"sampling",work,E::monitoring);
    const bool before=boolean(*member(*sampling,"before_observation",work,E::monitoring),work,E::monitoring);
    const bool after=boolean(*member(*sampling,"after_observation",work,E::monitoring),work,E::monitoring);
    const auto* during=member(*sampling,"during_observation",work,E::monitoring);
    const bool enabled=boolean(*member(*during,"enabled",work,E::monitoring),work,E::monitoring);
    work.debit();check(actual.before_observation()==before && policy.before_observation()==before,E::monitoring);
    work.debit();check(actual.after_observation()==after && policy.after_observation()==after,E::monitoring);
    work.debit();check(actual.during_observation()==enabled && policy.during_observation()==enabled,E::monitoring);
    work.debit();check(actual.during_interval().has_value()==enabled && policy.during_interval().has_value()==enabled,E::monitoring);
    if(enabled) {
        const U interval=number(*member(*member(*during,"interval",work,E::monitoring),"value",work,E::monitoring),work,E::monitoring);
        work.debit();check(actual.during_interval()->value()==interval && policy.during_interval()->value()==interval,E::monitoring);
    }
    const SV unavailable=string(*member(*root.monitoring,"unavailable_optional_source",work,E::monitoring),work,E::monitoring);
    if(work.equal(unavailable,"fail")) {
        work.debit();check(actual.unavailable_optional_source()==ipc::PlacementOptionalUnavailableSourcePolicy::fail && policy.optional_unsupported()==monitoring::OptionalUnavailableSourcePolicy::fail,E::monitoring);
    } else if(work.equal(unavailable,"warn")) {
        work.debit();check(actual.unavailable_optional_source()==ipc::PlacementOptionalUnavailableSourcePolicy::warn && policy.optional_unsupported()==monitoring::OptionalUnavailableSourcePolicy::warn,E::monitoring);
    } else {
        work.debit();check(work.equal(unavailable,"disable"),E::monitoring);
        work.debit();check(actual.unavailable_optional_source()==ipc::PlacementOptionalUnavailableSourcePolicy::disable && policy.optional_unsupported()==monitoring::OptionalUnavailableSourcePolicy::disable,E::monitoring);
    }
}
void reservation(const ipc::PlacementServiceReservation& actual,const Roots& root,Work& work) {
    const auto* cores=root.resource->reserved_service_cores();const auto* cpus=root.resource->reserved_service_cpus();
    work.debit();check(cores && cpus,E::state);
    const auto actual_cores=actual.stable_cores();const auto actual_cpus=actual.linux_cpus();
    work.debit();check(actual_cores.size()==cores->size(),E::service);
    const auto& effective=root.topology->machine().effective_cpus.value();
    work.debit();check(effective.has_value(),E::state);
    for(Z index=1;index<actual_cpus.size();++index) {
        work.debit();check(actual_cpus[index-1]<actual_cpus[index],E::service);
    }
    Z expected_count=0;
    for(Z index=0;index<actual_cores.size();++index) {
        work.debit();work.debit();check(work.equal(actual_cores[index].value(),(*cores)[index].value()),E::service);
        std::optional<core::CoreId> core;
        for(const auto& entry:root.stable_ids->cores()) {
            work.debit();
            if(work.equal(entry.semantic_id.value(),(*cores)[index].value())) {
                core=entry.handle;break;
            }
        }
        work.debit();check(core.has_value(),E::service);
        Z member_count=0;
        for(const auto& unit:root.topology->processing_units()) {
            work.debit();
            if(unit.core_id!=*core || !effective->contains(unit.linux_cpu_id))continue;
            work.debit();check(expected_count<actual_cpus.size(),E::service);
            Z lo=0,hi=actual_cpus.size();
            while(lo<hi) {
                work.debit();const Z mid=lo+(hi-lo)/2;
                if(actual_cpus[mid]<unit.linux_cpu_id)lo=mid+1;else hi=mid;
            }
            work.debit();check(lo<actual_cpus.size() && actual_cpus[lo]==unit.linux_cpu_id,E::service);
            ++expected_count;++member_count;
        }
        work.debit();check(member_count!=0,E::service);
    }
    work.debit();check(expected_count==actual_cpus.size(),E::service);
}
auto services(const detail::RequestView& inputs,const Roots& root,Work& work)
    ->std::array<ServiceLocator,2> {
    const auto assignments=inputs.services;
    const auto* cores=root.resource->reserved_service_cores();const auto* cpus=root.resource->reserved_service_cpus();
    const auto* pools=root.resource->service_cpu_pools();work.debit();check(cores && cpus && pools,E::state);
    work.debit();check(assignments.size()==(inputs.measurement_monitor_required?2U:1U),E::service);
    std::array<ServiceLocator,2> found{};std::array<bool,2> seen{};
    for(Z index=0;index<assignments.size();++index) {
        work.debit();const auto& assignment=assignments[index];
        work.debit();check(assignment.role==rt::ServiceRole::control_monitor || assignment.role==rt::ServiceRole::measurement_monitor,E::service);
        const Z role=assignment.role==rt::ServiceRole::control_monitor?0U:1U;
        work.debit();check(!seen[role],E::service);seen[role]=true;
        found[index].role=assignment.role;
        Z reserved_core=0;
        for(;reserved_core<cores->size();++reserved_core) {
            work.debit();
            if(work.equal((*cores)[reserved_core].value(),assignment.stable_core.value()))break;
        }
        work.debit();check(reserved_core<cores->size(),E::service);found[index].reserved_core=reserved_core;
        Z reserved_cpu=0;auto current=cpus->begin();const auto end=cpus->end();
        for(;current!=end;++current,++reserved_cpu) {
            work.debit();
            if(*current==assignment.linux_cpu)break;
        }
        work.debit();check(current!=end,E::service);found[index].reserved_cpu=reserved_cpu;
        Z pool_index=0;bool matched=false;
        auto pool=pools->cbegin();const auto end_pool=pools->cend();
        for(;pool!=end_pool && !matched;++pool,++pool_index) {
            work.debit();
            for(Z cpu=0;cpu<pool->second.processing_units.size();++cpu) {
                work.add(work.used.service_candidates,1,work.limits->service_candidates);work.debit();
                const auto& candidate=pool->second.processing_units[cpu];
                if(work.equal(candidate.stable_id.value(),assignment.stable_pu.value()) &&
                    work.equal(candidate.core_id.value(),assignment.stable_core.value()) && candidate.linux_cpu==assignment.linux_cpu) {
                    found[index].pool=pool_index;found[index].pool_cpu=cpu;matched=true;break;
                }
            }
        }
        work.debit();check(matched,E::service);
    }
    work.debit();check(seen[0] && seen[1]==inputs.measurement_monitor_required,E::service);
    if(assignments.size()==2) {work.debit();check(found[0].reserved_core!=found[1].reserved_core,E::service);}
    return found;
}
void side(const ipc::PlacementRequest& request,const rt::AdmittedPlacementRequest* admitted,const Roots& root,const Row& selected,
          U request_id,U seed,const monitoring::MonitoringExecutionPolicy& monitoring,Work& work) {
    const auto* provenance=std::get_if<rt::ScenarioPlacementProvenance>(&request.provenance());
    work.debit();check(provenance && request.transport_version()==ipc::PlacementRequestTransportVersion::v3,E::identity);
    work.debit();check(root.plan->scenario_digest() && root.plan->resolved_digest(),E::state);
    work.debit();check(provenance->scenario_digest()==*root.plan->scenario_digest(),E::identity);
    work.debit();check(provenance->resolved_scenario_plan_digest()==*root.plan->resolved_digest(),E::identity);
    const auto* fingerprints=root.resource->topology_fingerprints();work.debit();check(fingerprints,E::state);
    work.debit();check(request.topology().physical_fingerprint()==fingerprints->physical,E::identity);
    work.debit();check(request.topology().effective_fingerprint()==fingerprints->effective,E::identity);
    work.debit();check(!admitted || admitted->request_id().value()==request_id,E::identity);
    work.debit();check(request.experiment().index()==root.pair->experiment_index,E::identity);
    work.debit();check(work.equal(request.experiment().id().value(),selected.experiment),E::identity);
    work.debit();check(work.equal(request.experiment().workload_id().value(),selected.binding->workload_id),E::identity);
    work.debit();check(request.experiment().kernel_id()==selected.workload->kernel_id,E::identity);
    const auto expected_bytes=kernel(request.execution_inputs().kernel(),selected,work);
    const auto workers=admitted?admitted->workers():std::span<const rt::AdmittedWorker>{};const auto requested=request.workers();
    work.debit();check(requested.size()==selected.workers.size() && (!admitted || requested.size()==workers.size()),E::placement);
    for(Z index=0;index<requested.size();++index) {
        work.add(work.used.workers,1,work.limits->workers);work.debit();
        work.debit();check(!admitted || &workers[index].request()==&requested[index],E::placement);
        worker(requested[index].cpu(),selected.workers[index],work);
        work.debit();check(requested[index].region().bytes()==expected_bytes,E::placement);
        placement(requested[index],selected,index,work);
    }
    measurement(request.execution_inputs().measurement(),root,seed,work);
    work.debit();check(request.monitoring_inputs().has_value(),E::monitoring);
    monitoring_policy(*request.monitoring_inputs(),monitoring,root,work);
    reservation(request.reserved_service(),root,work);
}
auto compare_values(const Roots& root,const detail::RequestView& inputs,U seed,Work& work,
    const rt::AdmittedPlacementRequest* first=nullptr,const rt::AdmittedPlacementRequest* second=nullptr)
    ->std::array<ServiceLocator,2> {
    work.debit();check(root.topology && root.stable_ids && root.fingerprints,E::state);
    const auto* fingerprints=root.resource->topology_fingerprints();
    work.debit();check(fingerprints,E::state);
    work.debit();check(*root.fingerprints==*fingerprints,E::identity);
    work.debit();check(inputs.comparison_seed==root.pair->order_seed,E::identity);
    {
        const auto selected=row(root,0,work);
        side(inputs.first,first,root,selected,inputs.first_request,seed,inputs.monitoring,work);
        statistics(inputs,root,selected,work);
    }
    {
        const auto selected=row(root,1,work);
        side(inputs.second,second,root,selected,inputs.second_request,seed,inputs.monitoring,work);
    }
    return services(inputs,root,work);
}
auto roots(const detail::PlanView& view,Work& work)->Roots {
    Roots result;result.plan=&view.plan;result.pair=&view.pair;
    result.topology=&view.topology;result.stable_ids=&view.stable_ids;result.fingerprints=&view.fingerprints;
    result.selected=view.workloads;result.bindings=view.bindings;
    result.document=result.plan->document_ancestry();work.debit();check(result.document,E::state);
    result.resource=result.plan->resource_service();work.debit();check(result.resource,E::state);
    result.kernel=result.plan->kernel_dispatch();work.debit();check(result.kernel,E::state);
    result.comparisons=result.plan->comparison_experiments();work.debit();check(result.comparisons,E::state);
    result.matrices=result.plan->matrix_experiments();work.debit();check(result.matrices,E::state);
    result.normalized=result.document->unit_normalized();work.debit();check(result.normalized,E::state);
    result.effective=result.document->default_expanded();work.debit();check(result.effective,E::state);
    result.measurement=member(*result.effective,"measurement",work,E::state);
    result.monitoring=member(*result.effective,"monitoring",work,E::state,false);
    return result;
}
namespace inventory {
using AP=rt::AdmittedPlacementRequest;using PR=ipc::PlacementRequest;using A=rt::AdmittedPairwiseProductionInputs;
using P=ep::Pair;using R=Evidence;using Entry=process_subject::Entry;
struct FactualCompareCall {
    const detail::PlanView* plan;
    const detail::RequestView* request;
    const Roots* roots;
    const AP *first,*second;
    U seed;
    const Limits* limits;
    Usage* output;
    Work work;
    std::array<ServiceLocator,2> selected,returned;
    std::expected<std::array<ServiceLocator,2>,E> result;
};
struct RequestCall {
    std::span<const Entry> entries;U planning,ordinal,first,second;const A* inputs;const Limits* limits;Usage* output;
    Work work;Z lo,hi,mid;const ep::Owner* owner;const ep::Counts* counts;
    R assembled;std::expected<R,E> returned;std::optional<std::expected<R,E>> caller;
    std::array<ServiceLocator,2> services;
    detail::RequestView factual_request;
    const rt::RunnerContext* context;
};
struct RootsCall {const ep::Owner* owner;U ordinal;Work* work;std::span<const P> pairs;};
struct RowCall {
    const Roots* root;Z side,index;Work* work;const P* pair;
    std::span<const implementation_selection::Workload> selected;
    std::span<const implementation_selection::Binding> bindings;
    const kernel_dispatch_serialization::Workloads* workloads;const kernel_dispatch_serialization::Bindings* dispatched;
    kernel_dispatch_serialization::Workloads::const_iterator current,end;
    const std::vector<sc::ResolvedComparisonExperiment>* ordinary;
    const std::vector<sc::ExpandedMatrixExperiment>* matrices;
};
struct SideCall {
    const AP* admitted;const Roots* root;const Row* selected;U request_id,seed;
    const monitoring::MonitoringExecutionPolicy* monitoring;Work* work;const PR* request;
    const rt::ScenarioPlacementProvenance* provenance;const topology::TopologyFingerprints* fingerprints;
    core::ByteCount bytes;std::span<const rt::AdmittedWorker> workers;
    std::span<const ipc::PlacementWorkerRequest> requested;Z index;
};
struct WorkerCall {const ipc::PlacementCpuAssignment* actual;const sc::ResolvedProcessingUnit* expected;Work* work;};
struct NodeCall {const ipc::PlacementNumaTarget* actual;const sc::ResolvedMemoryNode* expected;Work* work;};
struct RegionCall {
    const ipc::PlacementRegionRequest* actual;sc::MemoryPolicy policy;sc::PageMode page;core::ByteCount size;
    const sc::ExactRatio* threshold;bool prefault,before,after;Work* work;
};
struct UniformCall {const ipc::PlacementRegionRequest* actual;const sc::ResolvedMemoryPlacement* expected;Work* work;std::span<const ipc::PlacementNumaTarget> targets;Z index;};
struct PlacementCall {
    const ipc::PlacementWorkerRequest* actual;const Row* expected;Z index;Work* work;
    const ipc::PlacementRegionRequest* memory;const sc::ResolvedMemoryPlacement* uniform;
    const sc::ResolvedPerWorkerMemoryPlacement* per_worker;const sc::ResolvedPerWorkerMemoryNode* mapping;
    std::span<const ipc::PlacementNumaTarget> targets;
};
struct KernelCall {
    const rt::AdmittedKernelExecutionInputs* actual;const Row* expected;Work* work;
    const kernel::SequentialReadBandwidthConfig* bw;const kernel::DependentLoadLatencyConfig* latency;
    const ipc::PlacementBandwidthExecutionInputs* bw_input;const ipc::PlacementLatencyExecutionInputs* lat_input;
    core::ByteCount returned;
};
struct MeasurementCall {
    const rt::AdmittedMeasurementInputs* actual;const Roots* root;U seed;Work* work;
    const J *observation,*calibration,*warmup,*repetition,*stopping;
};
struct StatisticsCall {
    const A* inputs;const Roots* root;const Row* selected;Work* work;
    const rt::StatisticsComparisonPolicySource* source;const rt::StatisticsProductionPolicy* projected;
    const J *statistics,*experiments,*experiment,*container,*local;const std::optional<sc::ExactRatio>* actual_local;
    const statistics::BootstrapPolicy *comparison,*stopping;core::DerivedSeed first_seed,second_seed;
};
struct ChannelCall {SV text;Work* work;Z returned;};
struct ChannelsCall {
    const J* source;std::span<const ipc::PlacementMonitoringChannel> actual;
    monitoring::MonitoringChannelDisposition disposition;
    std::array<monitoring::MonitoringChannelDisposition,4>* expected;Work* work;Z index,target;
};
struct MonitoringCall {
    const ipc::PlacementMonitoringInputs* actual;const monitoring::MonitoringExecutionPolicy* policy;
    const Roots* root;Work* work;std::array<monitoring::MonitoringChannelDisposition,4> expected;
    Z index;const J *sampling,*during;bool before,after,enabled;U interval;SV unavailable;
};
struct ReservationCall {
    const ipc::PlacementServiceReservation* actual;const Roots* root;Work* work;
    const std::vector<topology::StableCoreId>* cores;const core::CpuSet* cpus;
    std::span<const topology::StableCoreId> actual_cores;std::span<const core::LinuxCpuId> actual_cpus;
    const std::optional<core::CpuSet>* effective;
    std::optional<core::CoreId> core;
    const topology::StableCoreEntry* entry;
    const topology::ProcessingUnit* unit;
    Z index,expected_count,member_count,lo,hi,mid;
};
struct ServicesCall {
    const A* inputs;const Roots* root;Work* work;std::span<const rt::ServiceThreadAssignment> assignments;
    const std::vector<topology::StableCoreId>* cores;const core::CpuSet* cpus;
    const resource_service_serialization::PoolMap* pools;std::array<ServiceLocator,2> found,returned;
    std::array<bool,2> seen;Z index,role,reserved_core,reserved_cpu,pool_index,cpu;
    const rt::ServiceThreadAssignment* assignment;core::CpuSet::const_iterator current,end;
    resource_service_serialization::PoolMap::const_iterator pool,end_pool;bool matched;
    const sc::ResolvedServiceCpu* candidate;
};
struct MemberCall {const J* object;SV key;Work* work;E error;bool required;J::const_iterator current,end;const J* returned;};
struct JsonAccessCalls {
    const J* receiver;const J::number_unsigned_t* unsigned_return;const J::boolean_t* bool_return;
    const J::string_t* string_return;const J* indexed_return;
    const J::const_iterator* read_iterator;J::const_iterator* increment_iterator;
    J::const_iterator begin_return,end_return;const J::string_t* key_return;const J* value_return;
    Z index,size_return;bool type_return;
};
struct MapIteratorCalls {
    const kernel_dispatch_serialization::Workloads* workloads;
    const kernel_dispatch_serialization::Workloads::const_iterator* workload_read;
    kernel_dispatch_serialization::Workloads::const_iterator* workload_increment;
    kernel_dispatch_serialization::Workloads::const_iterator workload_begin,workload_end;
    const kernel_dispatch_serialization::Workloads::value_type* workload_return;
    const resource_service_serialization::PoolMap* pools;
    const resource_service_serialization::PoolMap::const_iterator* pool_read;
    resource_service_serialization::PoolMap::const_iterator* pool_increment;
    resource_service_serialization::PoolMap::const_iterator pool_begin,pool_end;
    const resource_service_serialization::PoolMap::value_type* pool_return;
};
struct NumberCall {const J* value;Work* work;E error;U returned;};
struct BooleanCall {const J* value;Work* work;E error;bool returned;};
struct StringCall {const J* value;Work* work;E error;SV returned;};
struct RatioCall {const J* value;const sc::ExactRatio* actual;Work* work;E error;const J *numerator,*denominator;};
struct WorkAdd {Work* receiver;Z* counter;Z amount,ceiling;};
struct WorkDebit {Work* receiver;Z amount;};
struct WorkCheck {bool predicate;E error;};
struct WorkEqual {Work* receiver;SV first,second;std::optional<Z> bytes,charge;bool returned;};
struct Arithmetic {Z first,second,result;std::optional<Z> returned;};
struct ErrorFlow {E thrown,caught;std::unexpected<E> returned;};
struct CpuRangeMirror {std::uint32_t first,last;};
struct CpuIteratorConstructor {core::CpuSet::const_iterator* receiver;const std::vector<CpuRangeMirror>* ranges;Z range_index;};
struct CpuIteratorCalls {
    const core::CpuSet* receiver;core::CpuSet::const_iterator begin_return,end_return;
    const CpuRangeMirror* range;core::LinuxCpuId dereference_return;
    const core::CpuSet::const_iterator* dereference_receiver;
    core::CpuSet::const_iterator* increment_receiver;
    CpuIteratorConstructor constructor;
};
// Typed getter frames reserve separate borrowed receivers/returns. Stable-name
// operands can coexist, so their returned string_views and receivers have two cells.
struct GetterReceivers {
    const ep::Owner* ep;const A* a;const AP* ap;const PR* pr;
    const rt::AdmittedPairwiseProductionRequest* pair;const rt::AdmittedExecutionInputs* execution;
    const rt::AdmittedMeasurementInputs* measurement;const rt::StatisticsComparisonPolicySource* source;
    const rt::StatisticsProductionPolicy* statistics;const monitoring::MonitoringExecutionPolicy* monitoring;
    const statistics::BootstrapPolicy* bootstrap;const rt::ServicePlacementPlan* service;
    const ipc::PlacementRegionRequest* region;const ipc::PlacementCpuAssignment* cpu;
    const ipc::PlacementNumaTarget* node;const ipc::PlacementWorkerRequest* worker;
    const ipc::PlacementExperimentRequest* experiment;const ipc::PlacementTopologyRequest* topology;
    const ipc::PlacementServiceReservation* reservation;const ipc::PlacementMonitoringInputs* input_monitoring;
    const rt::ScenarioPlacementProvenance* provenance;const sc::ExactRatio* exact;const ipc::PlacementRatio* ratio;
    const kernel::SequentialReadBandwidthConfig* bw;const kernel::DependentLoadLatencyConfig* latency;
    const kernel::PointerCycleSeedIntent* intent;
    const build_object_association::Owner* build;const implementation_selection::Owner* selection;
    const normalization_serialization::Owner* normalization;const private_plan_composition::Owner* plan;
    const document_ancestry_serialization::Owner* document;const resource_service_serialization::Owner* resource;
    const kernel_dispatch_serialization::Owner* kernel;const comparison_experiments_serialization::Owner* comparisons;
    const matrix_experiments_serialization::Owner* matrices;
};
struct GetterStrongChains {
    const ipc::RequestId* request;const core::LinuxCpuId* cpu;const core::LinuxNumaNodeId* node;
    const core::ByteCount* bytes;const core::Duration* duration;const core::DerivedSeed* seed;
    const statistics::BootstrapReplicateCount* replicate;const statistics::BootstrapConfidence* confidence;
    const topology::StableProcessingUnitId *pu_first,*pu_second;const topology::StableCoreId *core_first,*core_second;
    const topology::StableNumaNodeId *node_first,*node_second;const sc::ScenarioIdentifier* identifier;
    ipc::RequestId request_return;core::LinuxCpuId cpu_return;core::LinuxNumaNodeId node_return;
    core::ByteCount bytes_return;core::Duration duration_return;core::DerivedSeed seed_return;
    statistics::BootstrapReplicateCount replicate_return;statistics::BootstrapConfidence confidence_return;
    U number_return_first,number_return_second;double confidence_value_return;SV string_return_first,string_return_second;bool flag_return;
};
struct GetterEnums {
    sc::MatrixComparisonOrder order;ipc::PlacementCalibrationStrategy calibration;
    ipc::PlacementRequestTransportVersion transport;rt::StatisticsExperimentKind kind;
    kernel::KernelId kernel;kernel::PointerCycleSeedMode seed_mode;sc::PointerCycleSeedOrigin origin;
    ipc::PlacementPolicy placement;ipc::PlacementPageMode page;
    monitoring::MonitoringChannel channel;ipc::PlacementMonitoringChannel input_channel;
    monitoring::MonitoringChannelDisposition disposition;monitoring::OptionalUnavailableSourcePolicy unavailable;
    ipc::PlacementOptionalUnavailableSourcePolicy input_unavailable;std::optional<U> seed_intent;
};
struct GetterBorrowReturns {
    std::span<const P> pairs;std::span<const rt::ServiceThreadAssignment> assignments;
    std::span<const rt::AdmittedWorker> workers;std::span<const ipc::PlacementWorkerRequest> requested;
    std::span<const ipc::PlacementNumaTarget> targets;std::span<const ipc::PlacementMonitoringChannel> channels;
    std::span<const core::LinuxCpuId> cpus;std::span<const topology::StableCoreId> cores;
    const rt::AdmittedPairwiseProductionRequest* pair;const AP* admitted;const PR* request;
    const rt::AdmittedExecutionInputs* execution;const rt::AdmittedMeasurementInputs* measurement;
    const rt::AdmittedKernelExecutionInputs* kernel;const rt::StatisticsComparisonPolicySource* source;
    const rt::StatisticsProductionPolicy* statistics;const monitoring::MonitoringExecutionPolicy* monitoring;
    const rt::ServicePlacementPlan* service;const statistics::BootstrapPolicy* bootstrap;
    const ipc::PlacementRegionRequest* region;const ipc::PlacementCpuAssignment* cpu;
    const ipc::PlacementExperimentRequest* experiment;const ipc::PlacementTopologyRequest* topology;
    const ipc::PlacementServiceReservation* reservation;const std::optional<ipc::PlacementMonitoringInputs>* input_monitoring;
    const std::optional<core::Duration>* interval;const std::optional<sc::ExactRatio>* local;
    const sc::ExactRatio* exact;const ipc::PlacementRatio* ratio;
    const kernel::SequentialReadBandwidthConfig* bw;const kernel::DependentLoadLatencyConfig* latency;
    const kernel::PointerCycleSeedIntent* intent;const topology::StableProcessingUnitId* pu;
    const topology::StableCoreId* core;const topology::StableNumaNodeId* node;
    const sc::ScenarioIdentifier* identifier;const sc::ScenarioDigest* scenario_digest;
    const sc::ResolvedScenarioPlanDigest* resolved_digest;const topology::PhysicalTopologyFingerprint* physical;
    const topology::EffectiveTopologyFingerprint* effective;const rt::PlacementProvenance* provenance;
    const build_object_association::Owner* build;const implementation_selection::Owner* selection;
    const normalization_serialization::Owner* normalization;const private_plan_composition::Owner* plan;
    const document_ancestry_serialization::Owner* document;const resource_service_serialization::Owner* resource;
    const kernel_dispatch_serialization::Owner* dispatch;const comparison_experiments_serialization::Owner* comparisons;
    const matrix_experiments_serialization::Owner* matrices;const J* json;const ep::Counts* counts;
    const topology::TopologyFingerprints* fingerprints;
    std::span<const implementation_selection::Workload> selected;
    std::span<const implementation_selection::Binding> bindings;
    const kernel_dispatch_serialization::Workloads* workloads;const kernel_dispatch_serialization::Bindings* dispatched;
    const std::vector<sc::ResolvedComparisonExperiment>* ordinary;const std::vector<sc::ExpandedMatrixExperiment>* matrix;
    const std::vector<topology::StableCoreId>* reserved_cores;const core::CpuSet* reserved_cpus;
    const resource_service_serialization::PoolMap* pools;
};
struct EvidenceConstruction {
    R* destination;const A* source;U planning,pair,first,second;
    const rt::AdmittedMeasurementInputs* measurement;const rt::StatisticsComparisonPolicySource* statistics_source;
    const rt::StatisticsProductionPolicy* statistics;const monitoring::MonitoringExecutionPolicy* monitoring;
    std::array<ServiceLocator,2> services;Z count;std::optional<R> argument;
};
} // namespace inventory
} // namespace

namespace detail {
auto checked_add(Z first,Z second) noexcept->std::optional<Z> {
    if(second>std::numeric_limits<Z>::max()-first)return {};
    return first+second;
}
auto checked_multiply(Z first,Z second) noexcept->std::optional<Z> {
    if(first && second>std::numeric_limits<Z>::max()/first)return {};
    return first*second;
}
namespace {
consteval auto fixed_bytes() -> Z {
    using namespace inventory;
    constexpr Z cells[]={sizeof(RequestCall),sizeof(RootsCall),sizeof(Roots),sizeof(Roots),
        sizeof(RowCall),sizeof(Row),sizeof(Row),sizeof(SideCall),sizeof(WorkerCall),sizeof(NodeCall),
        sizeof(RegionCall),sizeof(UniformCall),sizeof(PlacementCall),sizeof(KernelCall),
        sizeof(MeasurementCall),sizeof(StatisticsCall),sizeof(ChannelCall),sizeof(ChannelsCall),
        sizeof(MonitoringCall),sizeof(ReservationCall),sizeof(ServicesCall),sizeof(MemberCall),
        sizeof(JsonAccessCalls),sizeof(MapIteratorCalls),sizeof(NumberCall),sizeof(BooleanCall),sizeof(StringCall),sizeof(RatioCall),sizeof(WorkAdd),
        sizeof(WorkDebit),sizeof(WorkCheck),sizeof(WorkEqual),sizeof(Arithmetic),sizeof(Arithmetic),
        sizeof(ErrorFlow),sizeof(CpuIteratorCalls),sizeof(GetterReceivers),sizeof(GetterStrongChains),
        sizeof(GetterEnums),sizeof(GetterBorrowReturns),sizeof(EvidenceConstruction),sizeof(FactualCompareCall)};
    Z sum=0;
    for(const Z cell:cells) {
        if(cell>std::numeric_limits<Z>::max()-sum)throw "fixed stock overflow";
        sum+=cell;
    }
    return sum;
}
}
auto fixed_stock() noexcept -> Z {return fixed_bytes();}

auto compare(const PlanView& plan,const RequestView& request,U seed,const Limits& limits,Usage& output) noexcept
    ->std::expected<std::array<ServiceLocator,2>,E> {
    try {
        Work work{output,&limits};
        work.debit();check(fixed_stock()<=limits.preparation_bytes,E::capacity);
        work.used.preparation_bytes=std::max(work.used.preparation_bytes,fixed_stock());
        work.add(work.used.records,1,limits.records);
        const auto root=roots(plan,work);
        const auto result=compare_values(root,request,seed,work);
        output=work.used;return result;
    } catch(E error) {return std::unexpected{error};}
}

auto prepare(std::span<const process_subject::Entry> entries,U planning,U ordinal,
             const rt::AdmittedPairwiseProductionInputs& inputs,U first,U second,
             const Limits& limits,Usage& output) noexcept->std::expected<Evidence,E> {
    try {
        Work work{output,&limits};
        work.debit();check(fixed_stock()<=limits.preparation_bytes,E::capacity);
        work.used.preparation_bytes=std::max(work.used.preparation_bytes,fixed_stock());work.add(work.used.records,1,limits.records);
        Z lo=0,hi=entries.size(),mid=0;
        while(lo<hi) {
            work.debit();mid=lo+(hi-lo)/2;
            work.debit();if(entries[mid].sequence<planning)lo=mid+1;else hi=mid;
        }
        work.debit();check(lo<entries.size() && entries[lo].sequence==planning && entries[lo].planning,E::reference);
        const auto* owner=&entries[lo].planning->plan;
        auto root=roots(*owner,ordinal,work);const auto* counts=owner->counts();work.debit();check(counts,E::state);
        const auto& context=inputs.pairwise_request().first().context();
        root.topology=&context.topology();root.stable_ids=&context.stable_ids();root.fingerprints=&context.fingerprints();
        const RequestView view{inputs.pairwise_request().first().request(),inputs.pairwise_request().second().request(),
            first,second,inputs.pairwise_request().resolved_comparison_order_seed(),inputs.statistics_source(),
            inputs.statistics_policy(),inputs.monitoring_policy(),inputs.service_placement().assignments(),
            inputs.measurement_monitor_required()};
        const auto selected_services=compare_values(root,view,counts->root_seed,work,
            &inputs.pairwise_request().first(),&inputs.pairwise_request().second());
        work.debit(4); // Four fixed policy values are copied only after all comparisons.
        Evidence result{planning,ordinal,first,second,inputs.pairwise_request().first().execution_inputs().measurement(),
            inputs.statistics_source(),inputs.statistics_policy(),inputs.monitoring_policy(),selected_services,
            inputs.service_placement().assignments().size()};
        output=work.used;return result;
    } catch(E error) {return std::unexpected{error};}
}
} // namespace detail
} // namespace epycbench::results::request_association
