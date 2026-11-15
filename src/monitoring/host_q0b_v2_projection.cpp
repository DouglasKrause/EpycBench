#include "epycbench/monitoring/host_q0b_v2_projection.hpp"
#include "core/json_ownership.hpp"
#include "monitoring/host_q0b_v2_projection_access.hpp"
#include "monitoring/host_runner_context_session_access.hpp"
#include "runtime/placement_provenance_access.hpp"

#include "monitoring/host_qualification_plan_access.hpp"
#include "monitoring/privacy_validation.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/build_manifest.hpp"
#include "epycbench/kernel/kernel_id.hpp"
#include "monitoring/host_q0b_c0_coordinator.hpp"
#include "monitoring/host_q0b_v2_test_access.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>

namespace epycbench::monitoring::qualification {
namespace {
thread_local std::optional<nlohmann::json> build_override;
thread_local bool synthetic_repository_build{};
thread_local detail::detail_HostQ0bV2TestAccess::Fault payload_fault{};
auto failure(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.host_q0b_v2.invalid_projection"},
            .message = "Monitoring host Q0b v2 projection failed",
            .operation = "project_monitoring_host_q0b_v2",
            .os_error = std::nullopt,
            .context = core::detail::json_object<nlohmann::json>([&](auto& json_value_64) {
                core::detail::set_json(json_value_64, "classification", std::move(classification));
            }),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
template<class T, class Maker> auto finish(Maker make, nlohmann::ordered_json value)
    -> std::expected<T,core::Error> {
    core::detail::JsonCleanup value_cleanup{value};
    if(!detail::privacy_safe(value)) return std::unexpected{failure("privacy_rejected")};
    auto canonical=core::canonicalize_json(value);
    core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
    if (!canonical)
        return std::unexpected{std::move(canonical.error())};
    while(!canonical->empty()&&canonical->back()=='\n')canonical->pop_back();
    canonical->push_back('\n');
    auto digest=core::sha256(*canonical);
    return make(std::move(value),std::move(*canonical),digest);
}
auto selected(const runtime::CalibrationOnlyProductionAdmission& admission,
              runtime::PairwiseVariant variant) -> const runtime::AdmittedPlacementRequest& {
    return variant==runtime::PairwiseVariant::first?admission.first():admission.second();
}
auto family_name(HostQ0bFamily family)->std::string_view {
    return family==HostQ0bFamily::bandwidth?"sequential-read-bandwidth":"dependent-load-latency";
}
auto expected_kernel(HostQ0bFamily family)->kernel::KernelId {
    return family==HostQ0bFamily::bandwidth?kernel::KernelId::sequential_read_bandwidth_v1:
                                           kernel::KernelId::dependent_load_latency_v1;
}
auto valid_qid(std::string_view value)->bool{
 constexpr std::string_view prefix="MON-007.host-v2.";
 if(!value.starts_with(prefix))return false;
 auto suffix=value.substr(prefix.size());
 if(suffix.empty()||suffix.size()>64U||suffix.front()=='-'||suffix.back()=='-')return false;
 return std::ranges::all_of(suffix,[](char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-';});
}
auto valid_timestamp(std::string_view value)->bool{
 if(value.size()!=20U||value[4]!='-'||value[7]!='-'||value[10]!='T'||value[13]!=':'||
    value[16]!=':'||value[19]!='Z')return false;
 const auto number=[&](std::size_t p,std::size_t n){unsigned result{};for(std::size_t i=p;i<p+n;++i){
   if(value[i]<'0'||value[i]>'9')return 10000U;
   result=result*10U+static_cast<unsigned>(value[i]-'0');
 }return result;};
 auto date=std::chrono::year_month_day{std::chrono::year{static_cast<int>(number(0,4))},
   std::chrono::month{number(5,2)},std::chrono::day{number(8,2)}};
 return date.ok()&&number(11,2)<=23U&&number(14,2)<=59U&&number(17,2)<=59U;
}
auto exact_keys(const nlohmann::json& value,std::initializer_list<std::string_view> keys)->bool{
 if(!value.is_object()||value.size()!=keys.size())return false;
 return std::ranges::all_of(keys,[&](auto key){return value.contains(key);});
}
template<class ServiceRange>
auto canonical_service_membership(const ServiceRange& services)
 ->std::expected<nlohmann::ordered_json,core::Error>{
 std::set<std::string> cores,pus;std::set<std::uint32_t> cpus;
 for(const auto& service:services){
   if(service.stable_pu_ids.empty()||service.stable_pu_ids.size()!=service.linux_cpus.size()||
      !cores.insert(std::string{service.stable_core_id.value()}).second)
     return std::unexpected{failure("service_binding_mismatch")};
   for(const auto& pu:service.stable_pu_ids)
     if(!pus.insert(std::string{pu.value()}).second)return std::unexpected{failure("service_binding_mismatch")};
   for(const auto cpu:service.linux_cpus)
     if(!cpus.insert(cpu.value()).second)return std::unexpected{failure("service_binding_mismatch")};
 }
 nlohmann::ordered_json value =
     core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_65) {
         core::detail::set_json(json_value_65, "stable_core_ids", nlohmann::ordered_json::array());
         core::detail::set_json(json_value_65, "stable_pu_ids", nlohmann::ordered_json::array());
         core::detail::set_json(json_value_65, "linux_cpus", nlohmann::ordered_json::array());
     });
 core::detail::JsonCleanup value_cleanup{value};
 for(const auto& core:cores)value["stable_core_ids"].push_back(core);
 for(const auto& pu:pus)value["stable_pu_ids"].push_back(pu);
 for(const auto cpu:cpus)value["linux_cpus"].push_back(cpu);
 return nlohmann::ordered_json(std::move(value));
}
auto valid_build(const nlohmann::json& build)->bool{
 if(!exact_keys(build,{"project_version","git","compiler","linker","preset","enabled_features","dependencies","build_identity"})||
    !build.at("git").is_object()||!build.at("compiler").is_object()||
    !build.at("linker").is_object()||!build.at("dependencies").is_object()||
    !exact_keys(build.at("git"),{"revision","dirty"})||!build.at("git").at("revision").is_string()||
    !build.at("git").at("dirty").is_boolean()||build.at("git").at("dirty").get<bool>()||
    !exact_keys(build.at("compiler"),{"id","version","target","cxx_standard","flags"})||
    build.at("compiler").at("cxx_standard")!="C++23"||
    !exact_keys(build.at("linker"),{"id","version"})||
    !exact_keys(build.at("dependencies"),{"hwloc","libnuma","libfyaml","CLI11","nlohmann-json","fmt","Catch2"})||
    !build.at("enabled_features").is_array()||!build.at("project_version").is_string()||
    !build.at("preset").is_string())return false;
 for(const auto key:{"id","version","target","flags"})if(!build.at("compiler").at(key).is_string())return false;
 for(const auto key:{"id","version"})if(!build.at("linker").at(key).is_string())return false;
 for(const auto& feature:build.at("enabled_features"))if(!feature.is_string())return false;
 std::set<std::string> features;for(const auto& feature:build.at("enabled_features"))
   if(!features.insert(feature.get<std::string>()).second)return false;
 for(const auto& [key,value]:build.at("dependencies").items()){
   (void)key;if(!value.is_string())return false;
 }
 const auto revision=build.at("git").at("revision").get<std::string>();
 const auto identity=build.at("build_identity");
 std::vector<std::string> expected_features{"linux","cxx23"};
 for(const auto feature:{"testing","asan_ubsan","tsan","native_reference"})
   if(features.contains(feature))expected_features.emplace_back(feature);
 if(build.at("enabled_features")!=expected_features)return false;
 std::string tuple;
 const auto append=[&](const std::string& value){if(!tuple.empty())tuple.push_back('|');tuple+=value;};
 append(build.at("project_version"));append(revision);append("false");
 for(const auto key:{"id","version","target","flags"})append(build.at("compiler").at(key));
 for(const auto key:{"id","version"})append(build.at("linker").at(key));
 append(build.at("preset"));
 for(const auto feature:{"testing","asan_ubsan","tsan","native_reference"})append(features.contains(feature)?"true":"false");
 for(const auto dependency:{"hwloc","libnuma","libfyaml","CLI11","nlohmann-json","fmt","Catch2"})
   append(build.at("dependencies").at(dependency));
 return (revision.size()==40U||revision.size()==64U)&&identity.is_string()&&identity==core::sha256(tuple).to_hex()&&
   std::ranges::all_of(revision,[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})&&
   std::ranges::all_of(identity.get_ref<const std::string&>(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
auto valid_test_build_override(const nlohmann::json& candidate,
                               const nlohmann::json& production)->bool {
 if(!valid_build(candidate)||!production.is_object())return false;
 for(const auto key:{"project_version","compiler","linker","preset","enabled_features","dependencies"})
   if(!production.contains(key)||candidate.at(key)!=production.at(key))return false;
 return production.contains("git")&&production.at("git").is_object()&&
   production.at("git").contains("revision")&&
   candidate.at("git").at("revision")==production.at("git").at("revision");
}
auto valid_family(const nlohmann::json& value,std::string_view family,
                  const core::Sha256Digest& facts,const core::Sha256Digest& base)->bool{
 if(!exact_keys(value,{"family","production_admission","selected_work","calibration","finalization"})||
    !value.at("production_admission").is_object()||!value.at("calibration").is_object()||
    value.at("family")!=family||!value.at("selected_work").is_number_unsigned()||
    value.at("selected_work").get<std::uint64_t>()==0U||
    !exact_keys(value.at("production_admission"),{"schema","sha256","variant_role","base_environment_sha256"})||
    value.at("production_admission").at("schema")!="epycbench.monitoring-admitted-production-facts/v2"||
    value.at("production_admission").at("sha256")!=facts.to_hex()||
    value.at("production_admission").at("variant_role")!="first"||
    value.at("production_admission").at("base_environment_sha256")!=base.to_hex()||
    value.at("finalization")!=nlohmann::json{{"operation","CalibrationOnlyProductionRun::finalize"},{"disposition","succeeded"}})
   return false;
 const auto& calibration=value.at("calibration");
 if(!exact_keys(calibration,{"path","duration_policy","empty_harness_trials","timer_overhead_estimate_ns","candidate_trials","disposition","selected_candidate"})||
    calibration.at("path")!="RT-009 production first-variant calibration"||calibration.at("disposition")!="selected"||
    !calibration.at("selected_candidate").is_number_unsigned()||calibration.at("selected_candidate").get<std::uint64_t>()==0U||
    !exact_keys(calibration.at("duration_policy"),{"minimum_duration_ns","target_duration_ns","maximum_duration_ns"})||
    !calibration.at("empty_harness_trials").is_array()||calibration.at("empty_harness_trials").empty()||
    !calibration.at("candidate_trials").is_array()||calibration.at("candidate_trials").empty())return false;
 const auto& duration=calibration.at("duration_policy");
 for(const auto key:{"minimum_duration_ns","target_duration_ns","maximum_duration_ns"})
   if(!duration.at(key).is_number_unsigned()||duration.at(key).get<std::uint64_t>()==0U)return false;
 if(duration.at("minimum_duration_ns").get<std::uint64_t>()>
      duration.at("target_duration_ns").get<std::uint64_t>()||
    duration.at("target_duration_ns").get<std::uint64_t>()>
      duration.at("maximum_duration_ns").get<std::uint64_t>()||
    !calibration.at("timer_overhead_estimate_ns").is_number_unsigned())return false;
 const auto valid_interval=[](const nlohmann::json& item,bool candidate){
   if(!exact_keys(item,candidate?std::initializer_list<std::string_view>{"candidate","start_ns","end_ns","elapsed_ns"}:
                                 std::initializer_list<std::string_view>{"start_ns","end_ns","elapsed_ns"}))return false;
   for(auto key:{"start_ns","end_ns","elapsed_ns"})if(!item.at(key).is_number_unsigned())return false;
   return !candidate||(item.at("candidate").is_number_unsigned()&&item.at("candidate").get<std::uint64_t>()>0U);
 };
 if(!std::ranges::all_of(calibration.at("empty_harness_trials"),[&](const auto& x){return valid_interval(x,false);})||
    !std::ranges::all_of(calibration.at("candidate_trials"),[&](const auto& x){return valid_interval(x,true);}))return false;
 const auto chosen=calibration.at("selected_candidate").get<std::uint64_t>();
 return value.at("selected_work").get<std::uint64_t>()==chosen&&
   std::ranges::any_of(calibration.at("candidate_trials"),[&](const nlohmann::json& x){
     return x.at("candidate").get<std::uint64_t>()==chosen;});
}
} // namespace

MonitoringHostAdmittedProductionFacts::~MonitoringHostAdmittedProductionFacts() noexcept {
    core::detail::retire_json(json_);
}
MonitoringHostQ0bBaseEnvironment::~MonitoringHostQ0bBaseEnvironment() noexcept {
    core::detail::retire_json(json_);
}

auto detail::HostQ0bProjectionAccess::facts(
    const RunnerContextAcquisitionSession& session, const MonitoringHostQualificationPlan& plan,
    const runtime::CalibrationOnlyProductionAdmission& admission, HostQ0bFamily family,
    runtime::PairwiseVariant variant, std::optional<core::Error>* raw_failure)
    -> std::expected<MonitoringHostAdmittedProductionFacts, core::Error> {
    if (!admission.has_value())
        return std::unexpected{failure("admission_context_mismatch")};
    if(family!=HostQ0bFamily::bandwidth&&family!=HostQ0bFamily::latency)
      return std::unexpected{failure("invalid_family")};
    if(variant!=runtime::PairwiseVariant::first&&variant!=runtime::PairwiseVariant::second)
      return std::unexpected{failure("invalid_variant")};
    auto session_binding = detail_HostQualificationPlanAccess::binding_fallible(session);
    core::detail::ExpectedErrorCleanup session_binding_cleanup{session_binding};
    if (!session_binding)
        return std::unexpected{std::move(session_binding.error())};
    if(*session_binding!=detail_HostQualificationPlanAccess::plan_binding(plan))
      return std::unexpected{failure("session_binding_mismatch")};
    auto session_context = detail_RunnerContextAcquisitionAccess::runner_context_fallible(session);
    core::detail::ExpectedErrorCleanup session_context_cleanup{session_context};
    if (!session_context)
        return std::unexpected{std::move(session_context.error())};
    if(&admission.first().context()!=&session_context->get()||
       &admission.second().context()!=&session_context->get())
      return std::unexpected{failure("admission_context_mismatch")};
    const auto& admitted=selected(admission,variant);
    const auto& request=admitted.request();
    auto provenance =
        runtime::detail::host_qualification_plan_provenance_fallible(request.provenance());
    core::detail::ExpectedErrorCleanup provenance_cleanup{provenance};
    if (!provenance) {
        if (raw_failure && !raw_failure->has_value())
            raw_failure->emplace(std::move(provenance.error()));
        return std::unexpected{failure("host_provenance_required")};
    }
    const auto& hp=provenance->get();
    if(request.transport_version()!=runtime::ipc::PlacementRequestTransportVersion::v4 ||
       request.experiment().kernel_id()!=expected_kernel(family) ||
       admitted.placement_identity().version()!=runtime::AdmittedPlacementIdentityVersion::v2)
      return std::unexpected{failure("family_binding_mismatch")};
    const auto expected_workers=family==HostQ0bFamily::bandwidth?plan.bandwidth_workers().size():1U;
    if(admitted.workers().size()!=expected_workers)return std::unexpected{failure("worker_cardinality")};
    const auto expected=family==HostQ0bFamily::bandwidth?plan.bandwidth_workers():plan.latency_workers();
    const auto bindings=family==HostQ0bFamily::bandwidth?plan.bandwidth_memory_bindings():plan.latency_memory_bindings();
    if(request.topology().physical_fingerprint()!=plan.physical_fingerprint()||
       request.topology().effective_fingerprint()!=plan.effective_fingerprint()||
       hp.policy_identity().text()!=detail_HostQualificationPlanAccess::policy_identity(plan)||
       hp.template_sha256()!=plan.template_sha256()||hp.environment_digest().text()!=plan.environment_digest()||
       hp.host_qualification_plan_digest().text()!=plan.plan_digest()||
       request.experiment().index()!=0U||request.experiment().id().value()!="q0b-pair"||
       request.experiment().workload_id().value()!="q0b-workload")
      return std::unexpected{failure("request_binding_mismatch")};
    nlohmann::ordered_json value =
        core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_52) {
            core::detail::set_json(
                json_value_52, "schema",
                nlohmann::ordered_json("epycbench.monitoring-admitted-production-facts/v2"));
            core::detail::set_json(
                json_value_52, "family",
                core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_53) {
                    core::detail::set_json(json_value_53, "family",
                                           nlohmann::ordered_json(family_name(family)));
                    core::detail::set_json(
                        json_value_53, "kernel_id",
                        nlohmann::ordered_json(kernel::kernel_id_token(expected_kernel(family))));
                    core::detail::set_json(
                        json_value_53, "worker_cardinality",
                        nlohmann::ordered_json(
                            family == HostQ0bFamily::bandwidth
                                ? nlohmann::ordered_json("one-per-eligible-numa-node")
                                : nlohmann::ordered_json(1)));
                }));
            core::detail::set_json(json_value_52, "variant_role",
                                   nlohmann::ordered_json(variant == runtime::PairwiseVariant::first
                                                              ? "first"
                                                              : "second"));
            core::detail::set_json(
                json_value_52, "provenance",
                core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_54) {
                    core::detail::set_json(json_value_54, "kind",
                                           nlohmann::ordered_json("host_qualification_plan"));
                    core::detail::set_json(json_value_54, "policy_identity",
                                           nlohmann::ordered_json(hp.policy_identity().text()));
                    core::detail::set_json(json_value_54, "template_sha256",
                                           nlohmann::ordered_json(hp.template_sha256().to_hex()));
                    core::detail::set_json(json_value_54, "environment_digest",
                                           nlohmann::ordered_json(hp.environment_digest().text()));
                    core::detail::set_json(
                        json_value_54, "host_qualification_plan_digest",
                        nlohmann::ordered_json(hp.host_qualification_plan_digest().text()));
                }));
            core::detail::set_json(
                json_value_52, "physical_fingerprint",
                nlohmann::ordered_json(request.topology().physical_fingerprint().to_string()));
            core::detail::set_json(
                json_value_52, "effective_fingerprint",
                nlohmann::ordered_json(request.topology().effective_fingerprint().to_string()));
            core::detail::set_json(
                json_value_52, "admitted_placement_identity",
                core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_55) {
                    core::detail::set_json(
                        json_value_55, "profile",
                        nlohmann::ordered_json("epycbench.admitted-placement-identity/v2"));
                    core::detail::set_json(
                        json_value_55, "sha256",
                        nlohmann::ordered_json(admitted.placement_identity().digest().to_hex()));
                }));
            core::detail::set_json(
                json_value_52, "experiment",
                core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_56) {
                    core::detail::set_json(json_value_56, "index",
                                           nlohmann::ordered_json(request.experiment().index()));
                    core::detail::set_json(
                        json_value_56, "id",
                        nlohmann::ordered_json(request.experiment().id().value()));
                    core::detail::set_json(
                        json_value_56, "workload_id",
                        nlohmann::ordered_json(request.experiment().workload_id().value()));
                    core::detail::set_json(json_value_56, "kernel_id",
                                           nlohmann::ordered_json(kernel::kernel_id_token(
                                               request.experiment().kernel_id())));
                }));
            core::detail::set_json(json_value_52, "workers", nlohmann::ordered_json::array());
            core::detail::set_json(
                json_value_52, "service_reservation",
                core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_57) {
                    core::detail::set_json(json_value_57, "stable_core_ids",
                                           nlohmann::ordered_json::array());
                    core::detail::set_json(json_value_57, "stable_pu_ids",
                                           nlohmann::ordered_json::array());
                    core::detail::set_json(json_value_57, "linux_cpus",
                                           nlohmann::ordered_json::array());
                }));
            core::detail::set_json(
                json_value_52, "admission_invariants",
                core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_58) {
                    core::detail::set_json(json_value_58, "admitted", nlohmann::ordered_json(true));
                    core::detail::set_json(json_value_58, "family_membership",
                                           nlohmann::ordered_json(true));
                    core::detail::set_json(json_value_58, "service_worker_disjoint",
                                           nlohmann::ordered_json(true));
                    core::detail::set_json(json_value_58, "topology_consistent",
                                           nlohmann::ordered_json(true));
                    core::detail::set_json(json_value_58, "worker_local_bind",
                                           nlohmann::ordered_json(true));
                }));
        });
    core::detail::JsonCleanup value_cleanup{value};
    for(std::size_t i=0;i<admitted.workers().size();++i){
      const auto& worker=admitted.workers()[i];const auto& wr=worker.request();
      if(wr.region().nodes().size()!=1U)return std::unexpected{failure("worker_region")};
      const auto& node=wr.region().nodes()[0];
      const auto binding=std::ranges::find_if(bindings,[&](const auto& item){
        return item.variant==(variant==runtime::PairwiseVariant::first?HostPlanVariantId::first:HostPlanVariantId::second)&&item.worker_id==i;});
      if(i>=expected.size()||binding==bindings.end()||expected[i].worker_id!=i||
         wr.cpu().stable_pu_id()!=expected[i].stable_pu_id||wr.cpu().stable_core_id()!=expected[i].stable_core_id||
         wr.cpu().linux_cpu()!=expected[i].linux_cpu||node.stable_numa_node_id()!=expected[i].stable_numa_node_id||
         node.linux_numa_node()!=binding->linux_node||wr.region().bytes()!=plan.working_set_bytes()||
         wr.region().page_size_bytes()!=plan.base_page_size()||wr.region().policy()!=runtime::ipc::PlacementPolicy::bind||
         wr.region().page_mode()!=runtime::ipc::PlacementPageMode::base||!wr.region().prefault()||
         !wr.region().verify_before()||!wr.region().verify_after()||
         wr.region().placement_threshold().numerator()!=999U||wr.region().placement_threshold().denominator()!=1000U)
        return std::unexpected{failure("worker_binding_mismatch")};
      core::detail::append_json(
          value["workers"],
          core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_66) {
              core::detail::set_json(json_value_66, "worker_id", nlohmann::ordered_json(i));
              core::detail::set_json(json_value_66, "stable_pu_id",
                                     nlohmann::ordered_json(wr.cpu().stable_pu_id().value()));
              core::detail::set_json(json_value_66, "stable_core_id",
                                     nlohmann::ordered_json(wr.cpu().stable_core_id().value()));
              core::detail::set_json(json_value_66, "linux_cpu",
                                     nlohmann::ordered_json(wr.cpu().linux_cpu().value()));
              core::detail::set_json(
                  json_value_66, "region",
                  core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_67) {
                      core::detail::set_json(json_value_67, "bytes",
                                             nlohmann::ordered_json(wr.region().bytes().value()));
                      core::detail::set_json(json_value_67, "policy",
                                             nlohmann::ordered_json("bind"));
                      core::detail::set_json(json_value_67, "page_mode",
                                             nlohmann::ordered_json("base"));
                      core::detail::set_json(
                          json_value_67, "page_size_bytes",
                          nlohmann::ordered_json(wr.region().page_size_bytes().value()));
                      core::detail::set_json(
                          json_value_67, "stable_numa_node_id",
                          nlohmann::ordered_json(node.stable_numa_node_id().value()));
                      core::detail::set_json(
                          json_value_67, "linux_numa_node",
                          nlohmann::ordered_json(node.linux_numa_node().value()));
                  }));
          }));
    }
    std::vector<std::string> admitted_cores,admitted_pus;
    std::vector<std::uint32_t> admitted_cpus;
    for(auto id:admitted.service().cores()){
      auto stable=admitted.context().stable_ids().core_id(id);if(!stable)return std::unexpected{failure("service_core")};
      admitted_cores.emplace_back(stable->get().value());
    }
    for(auto id:admitted.service().effective_processing_units()){
      auto stable=admitted.context().stable_ids().processing_unit_id(id);if(!stable)return std::unexpected{failure("service_pu")};
      admitted_pus.emplace_back(stable->get().value());
    }
    for(auto cpu:admitted.service().linux_cpus())admitted_cpus.push_back(cpu.value());
    std::ranges::sort(admitted_cores);std::ranges::sort(admitted_pus);std::ranges::sort(admitted_cpus);
    for(const auto& core:admitted_cores)value["service_reservation"]["stable_core_ids"].push_back(core);
    for(const auto& pu:admitted_pus)value["service_reservation"]["stable_pu_ids"].push_back(pu);
    for(const auto cpu:admitted_cpus)value["service_reservation"]["linux_cpus"].push_back(cpu);
    auto expected_service=canonical_service_membership(plan.service_cores());
    core::detail::ExpectedErrorCleanup expected_service_error_cleanup{expected_service};
    std::optional<core::detail::JsonCleanup<nlohmann::ordered_json>> expected_service_cleanup;
    if (expected_service)
        expected_service_cleanup.emplace(*expected_service);
    if (!expected_service && raw_failure && !*raw_failure)
        raw_failure->emplace(std::move(expected_service.error()));
    if(!expected_service||value["service_reservation"]!=*expected_service)
      return std::unexpected{failure("service_binding_mismatch")};
    return finish<MonitoringHostAdmittedProductionFacts>(
        [family, variant, binding = detail_HostQualificationPlanAccess::plan_binding(plan)](
            auto json, auto canonical, auto digest) {
            return MonitoringHostAdmittedProductionFacts{
                family, variant, binding, std::move(json), std::move(canonical), digest};
        },
        std::move(value));
}

auto project_monitoring_host_admitted_production_facts(
    const RunnerContextAcquisitionSession& session, const MonitoringHostQualificationPlan& plan,
    const runtime::CalibrationOnlyProductionAdmission& admission, HostQ0bFamily family,
    runtime::PairwiseVariant variant) noexcept
    -> std::expected<MonitoringHostAdmittedProductionFacts, core::Error> {
    try {
        return detail::HostQ0bProjectionAccess::facts(session, plan, admission, family, variant);
    } catch (const std::exception&) {
        return std::unexpected{failure("contained_exception")};
    } catch (...) {
        return std::unexpected{failure("contained_unknown_failure")};
    }
}
auto detail::HostQ0bProjectionAccess::base(const HostEnvironmentAuthority& environment,
                                           const MonitoringHostQualificationPlan& plan,
                                           const MonitoringHostAdmittedProductionFacts& bandwidth,
                                           const MonitoringHostAdmittedProductionFacts& latency,
                                           std::optional<core::Error>* raw_failure)
    -> std::expected<MonitoringHostQ0bBaseEnvironment, core::Error> {
    if (detail_HostQualificationPlanAccess::binding(environment) !=
        detail_HostQualificationPlanAccess::plan_binding(plan))
        return std::unexpected{failure("session_binding_mismatch")};
    if (bandwidth.binding_ != detail_HostQualificationPlanAccess::plan_binding(plan) ||
        latency.binding_ != detail_HostQualificationPlanAccess::plan_binding(plan))
        return std::unexpected{failure("facts_binding_mismatch")};
    const auto consistent = [&](const MonitoringHostAdmittedProductionFacts& facts) {
        if (!exact_keys(facts.json(),
                        {"schema", "family", "variant_role", "provenance", "physical_fingerprint",
                         "effective_fingerprint", "admitted_placement_identity", "experiment",
                         "workers", "service_reservation", "admission_invariants"}) ||
            !facts.json().at("provenance").is_object() ||
            !facts.json().at("service_reservation").is_object())
            return false;
        auto canonical = core::canonicalize_json(facts.json());
        core::detail::ExpectedErrorCleanup canonical_cleanup{canonical};
        if (!canonical) {
            if (raw_failure && !*raw_failure)
                raw_failure->emplace(std::move(canonical.error()));
            return false;
        }
        while (!canonical->empty() && canonical->back() == '\n')
            canonical->pop_back();
        canonical->push_back('\n');
        const auto& provenance = facts.json()["provenance"];
        return *canonical == facts.canonical_json() && core::sha256(*canonical) == facts.digest() &&
               facts.json()["physical_fingerprint"] ==
                   environment.physical_fingerprint().to_string() &&
               facts.json()["effective_fingerprint"] ==
                   environment.effective_fingerprint().to_string() &&
               provenance["environment_digest"] == environment.environment_digest() &&
               provenance["host_qualification_plan_digest"] == plan.plan_digest() &&
               provenance["template_sha256"] == plan.template_sha256().to_hex() &&
               provenance["policy_identity"] ==
                   detail_HostQualificationPlanAccess::policy_identity(plan);
    };
    if (!consistent(bandwidth) || !consistent(latency))
        return std::unexpected{failure("facts_canonical_binding_mismatch")};
    if (bandwidth.family() != HostQ0bFamily::bandwidth ||
        latency.family() != HostQ0bFamily::latency ||
        bandwidth.variant() != runtime::PairwiseVariant::first ||
        latency.variant() != runtime::PairwiseVariant::first)
        return std::unexpected{failure("family_binding_mismatch")};
    const auto& bs = bandwidth.json()["service_reservation"];
    auto expected_plan_service = canonical_service_membership(plan.service_cores());
    core::detail::ExpectedErrorCleanup expected_plan_error_cleanup{expected_plan_service};
    std::optional<core::detail::JsonCleanup<nlohmann::ordered_json>> expected_plan_cleanup;
    if (expected_plan_service)
        expected_plan_cleanup.emplace(*expected_plan_service);
    if (!expected_plan_service) {
        if (raw_failure && !*raw_failure)
            raw_failure->emplace(std::move(expected_plan_service.error()));
        return std::unexpected{failure("service_binding_mismatch")};
    }
    auto expected_environment_service = canonical_service_membership(environment.service_cores());
    core::detail::ExpectedErrorCleanup expected_environment_error_cleanup{
        expected_environment_service};
    std::optional<core::detail::JsonCleanup<nlohmann::ordered_json>> expected_environment_cleanup;
    if (expected_environment_service)
        expected_environment_cleanup.emplace(*expected_environment_service);
    if (!expected_environment_service && raw_failure && !*raw_failure)
        raw_failure->emplace(std::move(expected_environment_service.error()));
    if (!expected_plan_service || !expected_environment_service ||
        *expected_plan_service != *expected_environment_service ||
        bs != latency.json()["service_reservation"] || bs != *expected_plan_service)
        return std::unexpected{failure("service_binding_mismatch")};
    nlohmann::ordered_json value = core::detail::json_object<
        nlohmann::ordered_json>([&](auto& json_value_59) {
        core::detail::set_json(
            json_value_59, "schema",
            nlohmann::ordered_json("epycbench.monitoring-q0b-base-environment/v2"));
        core::detail::set_json(
            json_value_59, "policy_identity",
            nlohmann::ordered_json(detail_HostQualificationPlanAccess::policy_identity(plan)));
        core::detail::set_json(json_value_59, "template_sha256",
                               nlohmann::ordered_json(plan.template_sha256().to_hex()));
        core::detail::set_json(json_value_59, "environment_digest",
                               nlohmann::ordered_json(environment.environment_digest()));
        core::detail::set_json(json_value_59, "host_qualification_plan_digest",
                               nlohmann::ordered_json(plan.plan_digest()));
        core::detail::set_json(
            json_value_59, "physical_fingerprint",
            nlohmann::ordered_json(environment.physical_fingerprint().to_string()));
        core::detail::set_json(
            json_value_59, "effective_fingerprint",
            nlohmann::ordered_json(environment.effective_fingerprint().to_string()));
        core::detail::set_json(json_value_59, "stable_numa_order", nlohmann::ordered_json::array());
        core::detail::set_json(
            json_value_59, "service_reservation",
            core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_60) {
                core::detail::set_json(
                    json_value_60, "stable_core_ids",
                    core::detail::clone_json_as<nlohmann::ordered_json>(bs["stable_core_ids"]));
                core::detail::set_json(
                    json_value_60, "stable_pu_ids",
                    core::detail::clone_json_as<nlohmann::ordered_json>(bs["stable_pu_ids"]));
                core::detail::set_json(
                    json_value_60, "linux_cpus",
                    core::detail::clone_json_as<nlohmann::ordered_json>(bs["linux_cpus"]));
            }));
        core::detail::set_json(
            json_value_59, "families", ([&]() {
                auto json_value_61 = nlohmann::ordered_json::array();
                core::detail::JsonCleanup json_value_61_cleanup{json_value_61};
                core::detail::append_json(
                    json_value_61,
                    core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_62) {
                        core::detail::set_json(json_value_62, "family",
                                               nlohmann::ordered_json("sequential-read-bandwidth"));
                        core::detail::set_json(json_value_62, "admitted_production_sha256",
                                               nlohmann::ordered_json(bandwidth.digest().to_hex()));
                    }));
                core::detail::append_json(
                    json_value_61,
                    core::detail::json_object<nlohmann::ordered_json>([&](auto& json_value_63) {
                        core::detail::set_json(json_value_63, "family",
                                               nlohmann::ordered_json("dependent-load-latency"));
                        core::detail::set_json(json_value_63, "admitted_production_sha256",
                                               nlohmann::ordered_json(latency.digest().to_hex()));
                    }));
                return nlohmann::ordered_json(std::move(json_value_61));
            }()));
        core::detail::set_json(json_value_59, "topology_consistent", nlohmann::ordered_json(true));
        core::detail::set_json(json_value_59, "privacy_validation",
                               nlohmann::ordered_json("passed"));
    });
    core::detail::JsonCleanup value_cleanup{value};
    for (const auto& node : environment.eligible_nodes())
        value["stable_numa_order"].push_back(node.stable_numa_node_id.value());
    return finish<MonitoringHostQ0bBaseEnvironment>(
        [binding = detail_HostQualificationPlanAccess::plan_binding(plan),
         environment_digest = environment.environment_digest(), plan_digest = plan.plan_digest(),
         bandwidth_digest = bandwidth.digest(),
         latency_digest = latency.digest()](auto json, auto canonical, auto digest) {
            return MonitoringHostQ0bBaseEnvironment{
                binding,        environment_digest, plan_digest,          bandwidth_digest,
                latency_digest, std::move(json),    std::move(canonical), digest};
        },
        std::move(value));
}

auto project_monitoring_host_q0b_base_environment(
    const HostEnvironmentAuthority& environment, const MonitoringHostQualificationPlan& plan,
    const MonitoringHostAdmittedProductionFacts& bandwidth,
    const MonitoringHostAdmittedProductionFacts& latency) noexcept
    -> std::expected<MonitoringHostQ0bBaseEnvironment, core::Error> {
    try {
        return detail::HostQ0bProjectionAccess::base(environment, plan, bandwidth, latency);
    } catch (const std::exception&) {
        return std::unexpected{failure("contained_exception")};
    } catch (...) {
        return std::unexpected{failure("contained_unknown_failure")};
    }
}
auto build_monitoring_host_q0b_characterization(
 std::string_view qualification_id,std::string_view collected_at,
 const RunnerContextAcquisitionSession& session,const HostEnvironmentAuthority& environment,
 const MonitoringHostQualificationPlan& plan,const MonitoringHostQ0bBaseEnvironment& base,
 const MonitoringHostAdmittedProductionFacts& bandwidth,
 const MonitoringHostAdmittedProductionFacts& latency,const Q0bC0Calibrations& calibrations) noexcept
 ->std::expected<MonitoringHostQ0bCharacterization,core::Error>{
 try{
  auto authority_result=session.input_template();
  if(!authority_result)return std::unexpected{authority_result.error()};
  const auto& authority=authority_result->get();
  const auto production_build=build_manifest_json();
  auto build=build_override?*build_override:production_build;
  if(!valid_qid(qualification_id)||
     authority.policy_identity()!="epycbench.monitoring-qualification.host-generic/v2")
    return std::unexpected{failure("identity_mismatch")};
  if(!valid_timestamp(collected_at))return std::unexpected{failure("timestamp_invalid")};
  auto session_binding=detail_HostQualificationPlanAccess::binding(session);
  if(!session_binding)return std::unexpected{session_binding.error()};
  if(*session_binding!=detail_HostQualificationPlanAccess::plan_binding(plan)||
     base.binding_!=*session_binding||
     bandwidth.binding_!=base.binding_||latency.binding_!=base.binding_||
     base.environment_digest_!=environment.environment_digest()||base.plan_digest_!=plan.plan_digest()||
     base.bandwidth_digest_!=bandwidth.digest()||base.latency_digest_!=latency.digest()||
     detail_HostQualificationPlanAccess::policy_identity(plan)!=authority.policy_identity()||
     plan.template_sha256()!=authority.template_sha256())
    return std::unexpected{failure("authority_binding_mismatch")};
  if(!calibrations.families().is_array()||calibrations.families().size()!=2U||
     !valid_family(calibrations.families().at(0),"sequential-read-bandwidth",bandwidth.digest(),base.digest())||
     !valid_family(calibrations.families().at(1),"dependent-load-latency",latency.digest(),base.digest()))
    return std::unexpected{failure("calibration_binding_mismatch")};
  if(!valid_build(build)||(build_override&&!synthetic_repository_build&&
     !valid_test_build_override(build,production_build)))
    return std::unexpected{failure("build_invalid")};
  const auto revision=build.at("git").at("revision");
  const auto& service=plan.service_cores();
  if(service.size()!=2U)return std::unexpected{failure("service_binding_mismatch")};
  const auto& assignment=calibrations.control_monitor_assignment();
  if(assignment.role!=runtime::ServiceRole::control_monitor||
     assignment.stable_core!=service[0].stable_core_id||
     assignment.stable_pu!=service[0].execution_stable_pu_id||
     assignment.linux_cpu!=service[0].execution_linux_cpu)
    return std::unexpected{failure("control_assignment_mismatch")};
  nlohmann::ordered_json cores=nlohmann::ordered_json::array(),pus=nlohmann::ordered_json::array(),cpus=nlohmann::ordered_json::array();
  auto canonical_service=canonical_service_membership(service);
  if(!canonical_service)return std::unexpected{canonical_service.error()};
  cores=(*canonical_service)["stable_core_ids"];
  pus=(*canonical_service)["stable_pu_ids"];
  cpus=(*canonical_service)["linux_cpus"];
  nlohmann::ordered_json value{{"schema","epycbench.monitoring-work-characterization/v2"},
   {"qualification_id",qualification_id},{"policy",{{"identity",authority.policy_identity()},{"sha256",authority.policy_sha256().to_hex()}}},
   {"q0b_production_inputs",{{"schema","epycbench.monitoring-q0b-production-inputs/v2"},{"sha256",authority.manifest_sha256().to_hex()}}},
   {"host_environment",{{"schema","epycbench.monitoring-host-environment/v2"},{"digest",environment.environment_digest()}}},
   {"host_qualification_plan",{{"schema","epycbench.monitoring-host-qualification-plan/v2"},{"digest",plan.plan_digest()}}},
   {"q0b_base_environment",{{"schema","epycbench.monitoring-q0b-base-environment/v2"},{"sha256",base.digest().to_hex()}}},
   {"tool",{{"identity","epycbench.monitoring-qualification-tool/v1"},{"version","1.0.0"}}},
   {"repository",{{"commit",revision},{"dirty",false}}},{"build",std::move(build)},
   {"collected_at",collected_at},{"service_plan",{{"active_roles",nlohmann::ordered_json::array({"control"})},
    {"control_assignment",{{"stable_core_id",service[0].stable_core_id.value()},
      {"stable_pu_id",service[0].execution_stable_pu_id.value()},{"linux_cpu",service[0].execution_linux_cpu.value()}}},
    {"reserved_stable_core_ids",std::move(cores)},{"reserved_stable_pu_ids",std::move(pus)},
    {"reserved_linux_cpus",std::move(cpus)}}},
   {"monitoring",{{"configuration","C0"},{"optional",nlohmann::ordered_json::array()},
    {"required",nlohmann::ordered_json::array()},{"sampling",{{"after_observation",false},
      {"before_observation",false},{"during_observation",{{"enabled",false}}}}},
    {"unavailable_optional_source","fail"}}},{"families",calibrations.families()},
   {"provenance","preliminary_characterization"},{"pass_fail_eligible",false},
   {"checksums",{{"artifact","q0b/evidence.json"},{"host_environment","host-environment.json"},
     {"manifest","manifest.json"},{"checksums","checksums.sha256"}}},{"privacy_validation","passed"}};
  if(!detail::privacy_safe(value))return std::unexpected{failure("privacy_rejected")};
  auto canonical=core::canonicalize_json(value);if(!canonical)return std::unexpected{canonical.error()};
  while(!canonical->empty()&&canonical->back()=='\n')canonical->pop_back();
  canonical->push_back('\n');
  return MonitoringHostQ0bCharacterization{base.binding_,base.environment_digest_,base.plan_digest_,
    std::move(value),std::move(*canonical)};
 }catch(const std::exception&){return std::unexpected{failure("contained_exception")};}
 catch(...){return std::unexpected{failure("contained_unknown_failure")};}
}

auto build_monitoring_host_q0b_backend_result(const HostEnvironmentAuthority& environment,
 const MonitoringHostQ0bCharacterization& characterization) noexcept
 ->std::expected<MonitoringHostQ0bBackendResult,core::Error>{
 try{
  const std::initializer_list<std::string_view> characterization_keys{
    "schema","qualification_id","policy","q0b_production_inputs","host_environment",
    "host_qualification_plan","q0b_base_environment","tool","repository","build",
    "collected_at","service_plan","monitoring","families","provenance","pass_fail_eligible",
    "checksums","privacy_validation"};
  if(characterization.binding_!=detail_HostQualificationPlanAccess::binding(environment)||
     characterization.environment_digest_!=environment.environment_digest()||
     !exact_keys(characterization.json(),characterization_keys)||
     characterization.json().value("schema","")!=
     "epycbench.monitoring-work-characterization/v2"||
     !characterization.json().contains("host_environment")||
     !characterization.json().at("host_environment").is_object()||
     characterization.json().at("host_environment").value("digest","")!=environment.environment_digest()||
     !characterization.json().contains("host_qualification_plan")||
     !characterization.json().at("host_qualification_plan").is_object()||
     characterization.json().at("host_qualification_plan").value("digest","")!=characterization.plan_digest_||
     characterization.json().value("privacy_validation","")!="passed")
    return std::unexpected{failure("characterization_binding_mismatch")};
  auto recanonical=core::canonicalize_json(characterization.json());
  if(!recanonical)return std::unexpected{failure("characterization_malformed")};
  while(!recanonical->empty()&&recanonical->back()=='\n')recanonical->pop_back();
  recanonical->push_back('\n');
  if(*recanonical!=characterization.canonical_json()||characterization.canonical_json().size()>8U*1024U*1024U||
     core::sha256(environment.canonical_json()).to_hex()!=environment.digest().to_hex())
    return std::unexpected{failure("canonical_binding_mismatch")};
  auto host=nlohmann::ordered_json::parse(environment.canonical_json());
  nlohmann::ordered_json value{{"schema","epycbench.monitoring-host-q0b-backend-result/v2"},
    {"host_environment",std::move(host)},{"environment_digest",environment.environment_digest()},
    {"characterization",characterization.json()}};
  if(payload_fault==detail::detail_HostQ0bV2TestAccess::Fault::backend_oversize)
    value["characterization"]["qualification_id"]=std::string(10U*1024U*1024U,'x');
  if(payload_fault==detail::detail_HostQ0bV2TestAccess::Fault::backend_privacy)
    value["characterization"]["hostname"]="private-host";
  if(!detail::privacy_safe(value))return std::unexpected{failure("privacy_rejected")};
  auto canonical=core::canonicalize_json(value);if(!canonical)return std::unexpected{canonical.error()};
  while(!canonical->empty()&&canonical->back()=='\n')canonical->pop_back();
  canonical->push_back('\n');
  if(canonical->size()>10U*1024U*1024U)return std::unexpected{failure("transport_size_exceeded")};
  return MonitoringHostQ0bBackendResult{std::move(value),std::move(*canonical)};
 }catch(const std::exception&){return std::unexpected{failure("contained_exception")};}
 catch(...){return std::unexpected{failure("contained_unknown_failure")};}
}

void detail::detail_HostQ0bV2TestAccess::set_build_override(nlohmann::json value){
 synthetic_repository_build=false;build_override=std::move(value);
}
void detail::detail_HostQ0bV2TestAccess::set_synthetic_repository_build_override(nlohmann::json value){
 synthetic_repository_build=false;build_override=std::move(value);synthetic_repository_build=true;
}
void detail::detail_HostQ0bV2TestAccess::clear_build_override() noexcept{
 synthetic_repository_build=false;build_override.reset();
}
void detail::detail_HostQ0bV2TestAccess::set_fault(Fault value) noexcept{payload_fault=value;}
auto detail::detail_HostQ0bV2TestAccess::fault() noexcept->Fault{return payload_fault;}
auto detail::detail_HostQ0bV2TestAccess::mutable_json(
 MonitoringHostAdmittedProductionFacts& value) noexcept->nlohmann::ordered_json&{return value.json_;}
void detail::detail_HostQ0bV2TestAccess::mutate_canonical(
 MonitoringHostAdmittedProductionFacts& value,std::string canonical){value.canonical_=std::move(canonical);}
void detail::detail_HostQ0bV2TestAccess::mutate_digest(
 MonitoringHostAdmittedProductionFacts& value,core::Sha256Digest digest) noexcept{value.digest_=digest;}
void detail::detail_HostQ0bV2TestAccess::mutate_binding(MonitoringHostQ0bBaseEnvironment& value,
 std::shared_ptr<const detail_HostSessionBinding> binding) noexcept{value.binding_=std::move(binding);}
void detail::detail_HostQ0bV2TestAccess::mutate_environment_digest(
 MonitoringHostQ0bBaseEnvironment& value,std::string digest){value.environment_digest_=std::move(digest);}
void detail::detail_HostQ0bV2TestAccess::mutate_plan_digest(
 MonitoringHostQ0bBaseEnvironment& value,std::string digest){value.plan_digest_=std::move(digest);}
void detail::detail_HostQ0bV2TestAccess::mutate_family_digests(MonitoringHostQ0bBaseEnvironment& value,
 core::Sha256Digest bandwidth,core::Sha256Digest latency) noexcept{
 value.bandwidth_digest_=bandwidth;value.latency_digest_=latency;}
auto detail::detail_HostQ0bV2TestAccess::make_backend_result(
 nlohmann::ordered_json value,std::string canonical) noexcept->MonitoringHostQ0bBackendResult{
 return MonitoringHostQ0bBackendResult{std::move(value),std::move(canonical)};
}
} // namespace epycbench::monitoring::qualification
