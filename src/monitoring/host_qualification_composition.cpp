#include "monitoring/host_qualification_composition.hpp"
#include "monitoring/host_qualification_composition_test_access.hpp"

#include "core/json_ownership.hpp"
#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "runtime/calibration_only_production_admission_access.hpp"
#include "runtime/ipc/placement_request_construction_access.hpp"
#include "runtime/placement_provenance_access.hpp"
#include "runtime/production_request_id_access.hpp"

#include <algorithm>
#include <exception>
#include <ranges>
#include <set>
#include <stdexcept>

namespace epycbench::monitoring::qualification {
namespace { thread_local detail_HostQualificationCompositionAccess::Fault composition_fault{};
thread_local std::size_t bandwidth_admission_attempt_count{};
thread_local std::size_t latency_admission_attempt_count{}; }

auto detail_HostQualificationCompositionAccess::binding(const MonitoringHostQualificationPlan& plan) noexcept
 ->const std::shared_ptr<const detail_HostSessionBinding>& {return plan.binding_;}
auto detail_HostQualificationCompositionAccess::policy_identity(const MonitoringHostQualificationPlan& plan) noexcept
 ->const std::string& {return plan.policy_identity_;}
void detail_HostQualificationCompositionAccess::mutate_policy(MonitoringHostQualificationPlan& p,std::string v){p.policy_identity_=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_template(MonitoringHostQualificationPlan& p,core::Sha256Digest v){p.template_sha256_=v;}
void detail_HostQualificationCompositionAccess::mutate_environment(MonitoringHostQualificationPlan& p,std::string v){p.environment_digest_=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_physical(MonitoringHostQualificationPlan& p,topology::PhysicalTopologyFingerprint v){p.physical_=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_effective(MonitoringHostQualificationPlan& p,topology::EffectiveTopologyFingerprint v){p.effective_=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_working_set(MonitoringHostQualificationPlan& p,core::ByteCount v){p.working_set_bytes_=v;}
void detail_HostQualificationCompositionAccess::mutate_base_page(MonitoringHostQualificationPlan& p,core::ByteCount v){p.base_page_size_=v;}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_node(
 MonitoringHostQualificationPlan& p,core::LinuxNumaNodeId v){
 p.bandwidth_memory_bindings_[0].linux_node=v;p.bandwidth_memory_bindings_[1].linux_node=v;}
void detail_HostQualificationCompositionAccess::drop_latency_worker(MonitoringHostQualificationPlan& p){p.latency_workers_.clear();}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_worker_id(
 MonitoringHostQualificationPlan& p,std::uint32_t v){p.bandwidth_workers_[0].worker_id=v;
 p.bandwidth_memory_bindings_[0].worker_id=v;p.bandwidth_memory_bindings_[1].worker_id=v;}
void detail_HostQualificationCompositionAccess::drop_last_bandwidth_worker(
 MonitoringHostQualificationPlan& p){p.bandwidth_workers_.pop_back();}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_stable_pu(
 MonitoringHostQualificationPlan& p,topology::StableProcessingUnitId v){p.bandwidth_workers_[0].stable_pu_id=std::move(v);}
void detail_HostQualificationCompositionAccess::drop_service_core(MonitoringHostQualificationPlan& p){p.service_cores_.pop_back();}
void detail_HostQualificationCompositionAccess::mutate_canonical(MonitoringHostQualificationPlan& p,std::string v){p.canonical_=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_digest(MonitoringHostQualificationPlan& p,core::Sha256Digest v){p.digest_=v;}
void detail_HostQualificationCompositionAccess::swap_first_bandwidth_variants(MonitoringHostQualificationPlan& p){std::swap(p.bandwidth_memory_bindings_[0],p.bandwidth_memory_bindings_[1]);}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_linux_cpu(MonitoringHostQualificationPlan& p,core::LinuxCpuId v){p.bandwidth_workers_[0].linux_cpu=v;}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_worker_and_binding_cpu(
 MonitoringHostQualificationPlan& p,core::LinuxCpuId v){p.bandwidth_workers_[0].linux_cpu=v;
 p.bandwidth_memory_bindings_[0].linux_cpu=v;p.bandwidth_memory_bindings_[1].linux_cpu=v;}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_binding_cpu(MonitoringHostQualificationPlan& p,core::LinuxCpuId v){p.bandwidth_memory_bindings_[0].linux_cpu=v;p.bandwidth_memory_bindings_[1].linux_cpu=v;}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_stable_core(MonitoringHostQualificationPlan& p,topology::StableCoreId v){p.bandwidth_workers_[0].stable_core_id=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_first_bandwidth_stable_node(MonitoringHostQualificationPlan& p,topology::StableNumaNodeId v){p.bandwidth_workers_[0].stable_numa_node_id=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_first_service_role(MonitoringHostQualificationPlan& p,std::string v){p.service_cores_[0].role=std::move(v);}
void detail_HostQualificationCompositionAccess::mutate_first_service_core(MonitoringHostQualificationPlan& p,topology::StableCoreId v){p.service_cores_[0].stable_core_id=std::move(v);}
void detail_HostQualificationCompositionAccess::drop_first_service_stable_pu(MonitoringHostQualificationPlan& p){p.service_cores_[0].stable_pu_ids.pop_back();}
void detail_HostQualificationCompositionAccess::drop_first_service_linux_cpu(MonitoringHostQualificationPlan& p){p.service_cores_[0].linux_cpus.pop_back();}
void detail_HostQualificationCompositionAccess::mutate_active_service_role(MonitoringHostQualificationPlan& p,HostPlanServiceRole v){p.active_service_roles_[0]=v;}
void detail_HostQualificationCompositionAccess::set_fault(Fault value) noexcept{composition_fault=value;}
void detail_HostQualificationCompositionAccess::reset_admission_attempt_counts() noexcept{
 bandwidth_admission_attempt_count=0;latency_admission_attempt_count=0;}
auto detail_HostQualificationCompositionAccess::admission_attempt_counts() noexcept
 ->std::pair<std::size_t,std::size_t>{return {bandwidth_admission_attempt_count,
 latency_admission_attempt_count};}

namespace detail {
namespace {
using runtime::ipc::detail::PlacementRequestConstructionValues;

auto failure(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code =
                core::StableErrorCode{"monitoring.host_qualification_composition.binding_failed"},
            .message = "Monitoring host qualification composition binding failed",
            .operation = "compose_host_qualification_calibration_inputs",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json(std::move(classification)));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto contained(bool unknown) -> core::Error {
    auto value=failure(unknown?"contained_unknown_failure":"contained_exception");
    core::detail::JsonCleanup value_cleanup{value.context};
    value.code=core::StableErrorCode{"monitoring.host_qualification_composition.contained_failure"};
    return core::Error(std::move(value));
}
auto injected(std::string classification)->core::Error {
 auto value=failure(std::move(classification));
 core::detail::JsonCleanup value_cleanup{value.context};
 value.code=core::StableErrorCode{"monitoring.host_qualification_composition.injected_failure"};
 return core::Error(std::move(value));
}
auto allocation_failure()->core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{"runtime.production_request_id.allocation_failed"},
            .message = "Production request ID source is exhausted",
            .operation = "allocate_request_id",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([](auto& value) {
                core::detail::set_json(value, "classification",
                                       nlohmann::json("request_id_exhausted"));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto common_measurement(const MonitoringHostQualificationPlan& plan)
    -> runtime::ipc::detail::MeasurementValues {
    const auto& p=plan.calibration_policy();
    return {p.target_duration(),p.minimum_duration(),p.maximum_duration(),{1,100},true,
      runtime::ipc::PlacementCalibrationStrategy::geometric_refine,3,10,3,{3,100},8,20,16,
      false,{19,20},{1,50},scenario::MatrixComparisonOrder::randomized_abba,0};
}

auto provenance(const MonitoringHostQualificationPlan& plan)
    -> std::expected<runtime::PlacementProvenance,core::Error> {
    auto policy = runtime::detail::PlacementProvenanceConstructionAccess::policy_identity(
        detail_HostQualificationCompositionAccess::policy_identity(plan));
    core::detail::ExpectedErrorCleanup policy_cleanup{policy};
    if (!policy)
        return std::unexpected{std::move(policy.error())};
    auto environment = runtime::detail::PlacementProvenanceConstructionAccess::environment_digest(
        plan.environment_digest());
    core::detail::ExpectedErrorCleanup environment_cleanup{environment};
    if (!environment)
        return std::unexpected{std::move(environment.error())};
    auto digest =
        runtime::detail::PlacementProvenanceConstructionAccess::plan_digest(plan.plan_digest());
    core::detail::ExpectedErrorCleanup digest_cleanup{digest};
    if (!digest)
        return std::unexpected{std::move(digest.error())};
    return runtime::detail::PlacementProvenanceConstructionAccess::host(
      std::move(*policy),plan.template_sha256(),std::move(*environment),std::move(*digest));
}

auto service_cpus(const MonitoringHostQualificationPlan& plan) {
    std::vector<core::LinuxCpuId> result;
    for(const auto& service:plan.service_cores())
      result.insert(result.end(),service.linux_cpus.begin(),service.linux_cpus.end());
    std::ranges::sort(result);
    return result;
}
auto service_cores(const MonitoringHostQualificationPlan& plan) {
    std::vector<topology::StableCoreId> result;
    for(const auto& service:plan.service_cores())result.push_back(service.stable_core_id);
    std::ranges::sort(result,{},&topology::StableCoreId::value);
    return result;
}

auto workers(const MonitoringHostQualificationPlan& plan,bool bandwidth)
 ->std::expected<std::vector<runtime::ipc::detail::WorkerValues>,core::Error> {
    const auto selected=bandwidth?plan.bandwidth_workers():plan.latency_workers();
    const auto bindings=bandwidth?plan.bandwidth_memory_bindings():plan.latency_memory_bindings();
    std::vector<runtime::ipc::detail::WorkerValues> result;
    for(const auto& worker:selected) {
      const auto binding=std::ranges::find_if(bindings,[&](const auto& value){
        return value.variant==HostPlanVariantId::first && value.worker_id==worker.worker_id;
      });
      if(binding==bindings.end()||binding->linux_cpu!=worker.linux_cpu)
        return std::unexpected{failure("region_binding_mismatch")};
      runtime::ipc::detail::RegionValues region{plan.working_set_bytes(),
        runtime::ipc::PlacementPolicy::bind,
        {{worker.stable_numa_node_id,binding->linux_node}},runtime::ipc::PlacementPageMode::base,
        plan.base_page_size(),{999,1000},true,true,true};
      result.push_back({worker.stable_pu_id,worker.stable_core_id,worker.linux_cpu,std::move(region)});
    }
    return result;
}

auto validate_family(const MonitoringHostQualificationPlan& plan,bool bandwidth)
 ->std::expected<void,core::Error> {
   const auto selected=bandwidth?plan.bandwidth_workers():plan.latency_workers();
   const auto bindings=bandwidth?plan.bandwidth_memory_bindings():plan.latency_memory_bindings();
   if(bindings.size()!=selected.size()*2U)return std::unexpected{failure("variant_binding_mismatch")};
   for(std::size_t index=0;index<selected.size();++index) {
     const auto& worker=selected[index];
     const auto& first=bindings[index*2U];const auto& second=bindings[index*2U+1U];
     if(first.variant!=HostPlanVariantId::first||second.variant!=HostPlanVariantId::second||
        first.worker_id!=worker.worker_id||second.worker_id!=worker.worker_id||
        first.linux_cpu!=worker.linux_cpu||second.linux_cpu!=worker.linux_cpu||
        first.linux_node!=second.linux_node)
       return std::unexpected{failure("variant_binding_mismatch")};
   }
   return {};
}

auto make_request(const MonitoringHostQualificationPlan& plan,bool bandwidth,bool bad_topology=false)
 ->std::expected<runtime::ipc::PlacementRequest,core::Error> {
    auto experiment = scenario::ScenarioIdentifier::parse("q0b-pair");
    core::detail::ExpectedErrorCleanup experiment_cleanup{experiment};
    auto workload = scenario::ScenarioIdentifier::parse("q0b-workload");
    core::detail::ExpectedErrorCleanup workload_cleanup{workload};
    if(!experiment||!workload)return std::unexpected{failure("workload_binding_mismatch")};
    auto selected_workers = workers(plan, bandwidth);
    core::detail::ExpectedErrorCleanup selected_workers_cleanup{selected_workers};
    if (!selected_workers)
        return std::unexpected{std::move(selected_workers.error())};
    if(bandwidth) {
        auto config = kernel::SequentialReadBandwidthConfig::create(plan.working_set_bytes());
        core::detail::ExpectedErrorCleanup config_cleanup{config};
        if (!config)
            return std::unexpected{std::move(config.error())};
        auto source = provenance(plan);
        core::detail::ExpectedErrorCleanup source_cleanup{source};
        if (!source)
            return std::unexpected{std::move(source.error())};
        auto physical = plan.physical_fingerprint();
        if (bad_topology)
            physical =
                *topology::PhysicalTopologyFingerprint::parse("sha256:" + std::string(64, '1'));
        PlacementRequestConstructionValues values{
            runtime::ipc::PlacementRequestTransportVersion::v4,
            std::move(*source),
            std::move(physical),
            plan.effective_fingerprint(),
            0,
            std::move(*experiment),
            std::move(*workload),
            kernel::KernelId::sequential_read_bandwidth_v1,
            runtime::ipc::detail::BandwidthValues{std::move(*config), std::nullopt, std::nullopt},
            common_measurement(plan),
            runtime::ipc::detail::MonitoringValues{
                {},
                {},
                false,
                false,
                false,
                std::nullopt,
                runtime::ipc::PlacementOptionalUnavailableSourcePolicy::fail},
            service_cpus(plan),
            service_cores(plan),
            std::move(*selected_workers)};
        return runtime::ipc::detail::HostPlacementRequestConstructionAccess::construct(
            std::move(values));
    }
    auto config = kernel::DependentLoadLatencyConfig::create(
        plan.working_set_bytes(), kernel::PointerCycleSeedIntent::derive_per_experiment());
    core::detail::ExpectedErrorCleanup config_cleanup{config};
    if (!config)
        return std::unexpected{std::move(config.error())};
    auto source = provenance(plan);
    core::detail::ExpectedErrorCleanup source_cleanup{source};
    if (!source)
        return std::unexpected{std::move(source.error())};
    auto physical=plan.physical_fingerprint();
    if(bad_topology)physical=*topology::PhysicalTopologyFingerprint::parse(
      "sha256:"+std::string(64,'1'));
    PlacementRequestConstructionValues values{runtime::ipc::PlacementRequestTransportVersion::v4,
      std::move(*source),std::move(physical),plan.effective_fingerprint(),0,
      std::move(*experiment),std::move(*workload),kernel::KernelId::dependent_load_latency_v1,
      runtime::ipc::detail::LatencyValues{std::move(*config),13895703353266209705ULL,
        scenario::PointerCycleSeedOrigin::derived_value},
      common_measurement(plan),runtime::ipc::detail::MonitoringValues{{},{},false,false,false,
        std::nullopt,runtime::ipc::PlacementOptionalUnavailableSourcePolicy::fail},
      service_cpus(plan),service_cores(plan),std::move(*selected_workers)};
    return runtime::ipc::detail::HostPlacementRequestConstructionAccess::construct(std::move(values));
}
} // namespace

auto compose_host_qualification_calibration_inputs_fallible(
    const MonitoringHostQualificationPlan& plan, const RunnerContextAcquisitionSession& session,
    runtime::PairwiseProductionCompositionContext& context, bool source_route,
    std::optional<core::Error>* raw_failure)
    -> std::expected<HostQualificationCalibrationInputs, core::Error> {
    using Fault = detail_HostQualificationCompositionAccess::Fault;
    if (composition_fault == Fault::standard_exception)
        throw std::runtime_error{"private test"};
    if (composition_fault == Fault::unknown_exception)
        throw 7;
    auto session_binding = detail_HostQualificationPlanAccess::binding_fallible(session);
    core::detail::ExpectedErrorCleanup session_binding_cleanup{session_binding};
    if (!session_binding) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(session_binding.error()));
        return std::unexpected{failure("session_mismatch")};
    }
    auto session_context = detail_RunnerContextAcquisitionAccess::runner_context_fallible(session);
    core::detail::ExpectedErrorCleanup session_context_cleanup{session_context};
    if (!session_context) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(session_context.error()));
        return std::unexpected{failure("session_mismatch")};
    }
    auto input = detail_RunnerContextAcquisitionAccess::input_template_fallible(session);
    core::detail::ExpectedErrorCleanup input_cleanup{input};
    if (!input) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(input.error()));
        return std::unexpected{failure("policy_identity_mismatch")};
    }
    if (!session_binding || !session_context ||
        session_binding->get() != detail_HostQualificationCompositionAccess::binding(plan).get() ||
        &session_context->get() != &context.runner_context())
        return std::unexpected{failure("session_mismatch")};
    if (!input || detail_HostQualificationCompositionAccess::policy_identity(plan) !=
                      input->get().policy_identity())
        return std::unexpected{failure("policy_identity_mismatch")};
    if (plan.template_sha256() != input->get().template_sha256())
        return std::unexpected{failure("template_digest_mismatch")};
    if (plan.environment_digest() !=
        detail_HostQualificationPlanAccess::environment_digest(*session_binding))
        return std::unexpected{failure("environment_digest_mismatch")};
    if (plan.physical_fingerprint() != context.runner_context().fingerprints().physical)
        return std::unexpected{failure("physical_fingerprint_mismatch")};
    if (plan.effective_fingerprint() != context.runner_context().fingerprints().effective)
        return std::unexpected{failure("effective_fingerprint_mismatch")};
    if (composition_fault == Fault::descriptor_id_observation ||
        plan.bandwidth_descriptor().kernel_id() != kernel::KernelId::sequential_read_bandwidth_v1 ||
        plan.latency_descriptor().kernel_id() != kernel::KernelId::dependent_load_latency_v1)
        return std::unexpected{failure("descriptor_binding_mismatch")};
    if (composition_fault == Fault::descriptor_implementation_observation ||
        plan.bandwidth_descriptor().implementation_id() !=
            kernel::KernelImplementationId::scalar_u64_sequential_v1 ||
        plan.latency_descriptor().implementation_id() !=
            kernel::KernelImplementationId::scalar_u64_random_cycle_v1)
        return std::unexpected{failure("kernel_configuration_mismatch")};
    if (plan.working_set_bytes().value() != 1073741824U)
        return std::unexpected{failure("workload_binding_mismatch")};
    const auto& measurement = plan.calibration_policy();
    if (measurement.minimum_duration().value() != 250000000U ||
        measurement.target_duration().value() != 500000000U ||
        composition_fault == Fault::duration_observation ||
        measurement.maximum_duration().value() != 1000000000U)
        return std::unexpected{failure("measurement_binding_mismatch")};
    auto bandwidth_family = validate_family(plan, true);
    core::detail::ExpectedErrorCleanup bandwidth_family_cleanup{bandwidth_family};
    if (!bandwidth_family)
        return std::unexpected{std::move(bandwidth_family.error())};
    auto latency_family = validate_family(plan, false);
    core::detail::ExpectedErrorCleanup latency_family_cleanup{latency_family};
    if (!latency_family)
        return std::unexpected{std::move(latency_family.error())};
    if (plan.memory_policy() != HostPlanMemoryPolicy::bind || !plan.worker_local_memory())
        return std::unexpected{failure("memory_policy_mismatch")};
    if (plan.base_page_size().value() == 0U)
        return std::unexpected{failure("base_page_mismatch")};
    auto retained = detail_HostQualificationPlanAccess::requirements_fallible(session);
    core::detail::ExpectedErrorCleanup retained_cleanup{retained};
    if (!retained) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(retained.error()));
        return std::unexpected{failure("session_mismatch")};
    }
    const auto regions_match = [&](std::string_view family,
                                   std::span<const HostEnvironmentWorker> selected,
                                   std::span<const HostPlanWorkerMemoryBinding> bindings) {
        std::vector<std::reference_wrapper<const detail_HostSelectedWorkerRequirement>> expected;
        for (const auto& value : retained->get().workers)
            if (value.family == family)
                expected.push_back(std::cref(value));
        if (expected.size() != selected.size())
            return false;
        for (std::size_t index = 0; index < selected.size(); ++index) {
            const auto& worker = selected[index];
            const auto& required = expected[index].get();
            const auto first = std::ranges::find_if(bindings, [&](const auto& value) {
                return value.variant == HostPlanVariantId::first &&
                       value.worker_id == worker.worker_id;
            });
            if (first == bindings.end() || first->linux_node != required.node)
                return false;
        }
        return true;
    };
    if (!regions_match("bandwidth", plan.bandwidth_workers(), plan.bandwidth_memory_bindings()) ||
        !regions_match("latency", plan.latency_workers(), plan.latency_memory_bindings()))
        return std::unexpected{failure("region_binding_mismatch")};
    if (plan.monitoring_configuration() != HostPlanMonitoringConfiguration::c0 ||
        plan.active_service_roles().size() != 1U ||
        plan.active_service_roles()[0] != HostPlanServiceRole::control)
        return std::unexpected{failure("monitoring_binding_mismatch")};
    const auto matches_family = [&](std::string_view family,
                                    std::span<const HostEnvironmentWorker> selected) {
        std::vector<std::reference_wrapper<const detail_HostSelectedWorkerRequirement>> expected;
        for (const auto& value : retained->get().workers)
            if (value.family == family)
                expected.push_back(std::cref(value));
        if (expected.size() != selected.size())
            return false;
        for (std::size_t i = 0; i < selected.size(); ++i) {
            const auto& left = selected[i];
            const auto& right = expected[i].get();
            if (left.worker_id != right.worker_id ||
                left.stable_numa_node_id != right.stable_node ||
                left.stable_core_id != right.stable_core || left.stable_pu_id != right.stable_pu ||
                left.linux_cpu != right.cpu)
                return false;
        }
        return true;
    };
    if (plan.bandwidth_workers().empty() || plan.latency_workers().size() != 1 ||
        plan.latency_workers()[0].worker_id != plan.bandwidth_workers()[0].worker_id ||
        plan.latency_workers()[0].stable_pu_id != plan.bandwidth_workers()[0].stable_pu_id ||
        plan.latency_workers()[0].stable_core_id != plan.bandwidth_workers()[0].stable_core_id ||
        plan.latency_workers()[0].stable_numa_node_id !=
            plan.bandwidth_workers()[0].stable_numa_node_id ||
        plan.latency_workers()[0].linux_cpu != plan.bandwidth_workers()[0].linux_cpu)
        return std::unexpected{failure("worker_binding_mismatch")};
    for (std::size_t i = 0; i < plan.bandwidth_workers().size(); ++i)
        if (plan.bandwidth_workers()[i].worker_id != i)
            return std::unexpected{failure("worker_binding_mismatch")};
    if (plan.latency_workers()[0].worker_id != 0U)
        return std::unexpected{failure("worker_binding_mismatch")};
    if (!matches_family("bandwidth", plan.bandwidth_workers()) ||
        !matches_family("latency", plan.latency_workers()))
        return std::unexpected{failure("worker_binding_mismatch")};
    if (plan.service_cores().size() != 2 || plan.service_cores()[0].role != "control" ||
        plan.service_cores()[1].role != "measurement" ||
        plan.service_cores()[0].stable_core_id == plan.service_cores()[1].stable_core_id)
        return std::unexpected{failure("service_binding_mismatch")};
    if (retained->get().service.size() != plan.service_cores().size())
        return std::unexpected{failure("service_binding_mismatch")};
    std::set<std::string> service_core_ids;
    std::set<std::string> service_pu_ids;
    std::set<std::uint32_t> service_linux_cpus;
    for (std::size_t i = 0; i < plan.service_cores().size(); ++i) {
        const auto& left = plan.service_cores()[i];
        const auto& right = retained->get().service[i];
        if (left.role != right.role || left.stable_core_id != right.stable_core ||
            left.stable_pu_ids != right.stable_pus || left.linux_cpus != right.sibling_cpus ||
            left.stable_pu_ids.empty() || left.stable_pu_ids.size() != left.linux_cpus.size() ||
            !service_core_ids.insert(std::string{left.stable_core_id.value()}).second)
            return std::unexpected{failure("service_binding_mismatch")};
        const auto execution = std::ranges::find(left.stable_pu_ids, left.execution_stable_pu_id);
        if (execution == left.stable_pu_ids.end() ||
            left.linux_cpus[static_cast<std::size_t>(execution - left.stable_pu_ids.begin())] !=
                left.execution_linux_cpu)
            return std::unexpected{failure("service_binding_mismatch")};
        for (const auto& pu : left.stable_pu_ids)
            if (!service_pu_ids.insert(std::string{pu.value()}).second)
                return std::unexpected{failure("service_binding_mismatch")};
        for (const auto cpu : left.linux_cpus)
            if (!service_linux_cpus.insert(cpu.value()).second)
                return std::unexpected{failure("service_binding_mismatch")};
    }
    auto canonical = detail_HostQualificationPlanAccess::canonical_projection(plan);
    core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    if (!canonical && raw_failure && !*raw_failure)
        raw_failure->emplace(std::move(canonical.error()));
    if (!canonical || *canonical != plan.canonical_json())
        return std::unexpected{failure("typed_projection_incoherent")};
    if (core::sha256(plan.canonical_json()) != plan.digest())
        return std::unexpected{failure("canonical_digest_incoherent")};

    if (composition_fault == Fault::projection_1)
        return std::unexpected{injected("projection_1")};
    auto bw_first = make_request(plan, true);
    runtime::ipc::detail::HostPlacementRequestResultCleanup bw_first_cleanup{bw_first};
    if (!bw_first)
        return std::unexpected{std::move(bw_first.error())};
    if (composition_fault == Fault::projection_2)
        return std::unexpected{injected("projection_2")};
    auto bw_second = make_request(plan, true);
    runtime::ipc::detail::HostPlacementRequestResultCleanup bw_second_cleanup{bw_second};
    if (!bw_second)
        return std::unexpected{std::move(bw_second.error())};
    if (composition_fault == Fault::projection_3)
        return std::unexpected{injected("projection_3")};
    auto lat_first = make_request(plan, false);
    runtime::ipc::detail::HostPlacementRequestResultCleanup lat_first_cleanup{lat_first};
    if (!lat_first)
        return std::unexpected{std::move(lat_first.error())};
    if (composition_fault == Fault::projection_4)
        return std::unexpected{injected("projection_4")};
    auto lat_second = make_request(plan, false);
    runtime::ipc::detail::HostPlacementRequestResultCleanup lat_second_cleanup{lat_second};
    if (!lat_second)
        return std::unexpected{std::move(lat_second.error())};
    if (composition_fault == Fault::allocation_1)
        return std::unexpected{allocation_failure()};
    auto bw_first_id = runtime::detail::ProductionRequestIdAccess::allocate(context.request_ids());
    core::detail::ExpectedErrorCleanup bw_first_id_cleanup{bw_first_id};
    if (!bw_first_id)
        return std::unexpected{std::move(bw_first_id.error())};
    if (composition_fault == Fault::allocation_2)
        return std::unexpected{allocation_failure()};
    auto bw_second_id = runtime::detail::ProductionRequestIdAccess::allocate(context.request_ids());
    core::detail::ExpectedErrorCleanup bw_second_id_cleanup{bw_second_id};
    if (!bw_second_id)
        return std::unexpected{std::move(bw_second_id.error())};
    if (composition_fault == Fault::allocation_3)
        return std::unexpected{allocation_failure()};
    auto lat_first_id = runtime::detail::ProductionRequestIdAccess::allocate(context.request_ids());
    core::detail::ExpectedErrorCleanup lat_first_id_cleanup{lat_first_id};
    if (!lat_first_id)
        return std::unexpected{std::move(lat_first_id.error())};
    if (composition_fault == Fault::allocation_4)
        return std::unexpected{allocation_failure()};
    auto lat_second_id =
        runtime::detail::ProductionRequestIdAccess::allocate(context.request_ids());
    core::detail::ExpectedErrorCleanup lat_second_id_cleanup{lat_second_id};
    if (!lat_second_id)
        return std::unexpected{std::move(lat_second_id.error())};
    if (composition_fault == Fault::first_single_admission) {
        auto replacement = make_request(plan, true, true);
        runtime::ipc::detail::HostPlacementRequestResultCleanup replacement_cleanup{replacement};
        if (!replacement)
            return std::unexpected{std::move(replacement.error())};
        runtime::ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(*bw_first);
        bw_first = std::move(*replacement);
    }
    if (composition_fault == Fault::second_single_admission) {
        auto replacement = make_request(plan, true, true);
        runtime::ipc::detail::HostPlacementRequestResultCleanup replacement_cleanup{replacement};
        if (!replacement)
            return std::unexpected{std::move(replacement.error())};
        runtime::ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(*bw_second);
        bw_second = std::move(*replacement);
    }
    if (composition_fault == Fault::bandwidth_compatibility) {
        auto replacement = make_request(plan, false);
        runtime::ipc::detail::HostPlacementRequestResultCleanup replacement_cleanup{replacement};
        if (!replacement)
            return std::unexpected{std::move(replacement.error())};
        runtime::ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(*bw_second);
        bw_second = std::move(*replacement);
    }
    ++bandwidth_admission_attempt_count;
    if (composition_fault == Fault::bandwidth_admission)
        return std::unexpected{injected("bandwidth_admission")};
    auto bw = runtime::detail::HostCalibrationOnlyProductionAdmissionAccess::create_fallible(
        std::move(*bw_first), std::move(*bw_second), *bw_first_id, *bw_second_id, 0,
        context.runner_context(), source_route, raw_failure);
    core::detail::ExpectedErrorCleanup bw_cleanup{bw};
    if (!bw)
        return std::unexpected{std::move(bw.error())};
    if (composition_fault == Fault::latency_compatibility) {
        auto replacement = make_request(plan, true);
        runtime::ipc::detail::HostPlacementRequestResultCleanup replacement_cleanup{replacement};
        if (!replacement)
            return std::unexpected{std::move(replacement.error())};
        runtime::ipc::detail::HostPlacementRequestConstructionAccess::retire_wire(*lat_second);
        lat_second = std::move(*replacement);
    }
    ++latency_admission_attempt_count;
    if (composition_fault == Fault::latency_admission)
        return std::unexpected{injected("latency_admission")};
    auto lat = runtime::detail::HostCalibrationOnlyProductionAdmissionAccess::create_fallible(
        std::move(*lat_first), std::move(*lat_second), *lat_first_id, *lat_second_id, 0,
        context.runner_context(), source_route, raw_failure);
    core::detail::ExpectedErrorCleanup lat_cleanup{lat};
    if (!lat)
        return std::unexpected{std::move(lat.error())};
    return HostQualificationCalibrationInputs{std::move(*bw), std::move(*lat)};
}

auto compose_host_qualification_calibration_inputs(
    const MonitoringHostQualificationPlan& plan, const RunnerContextAcquisitionSession& session,
    runtime::PairwiseProductionCompositionContext& context) noexcept
    -> std::expected<HostQualificationCalibrationInputs, core::Error> {
    try {
        return compose_host_qualification_calibration_inputs_fallible(plan, session, context,
                                                                      false);
    } catch (const std::exception&) {
        return std::unexpected{contained(false)};
    } catch (...) {
        return std::unexpected{contained(true)};
    }
}
} // namespace detail
} // namespace epycbench::monitoring::qualification
