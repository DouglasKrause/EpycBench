#include "monitoring/host_q0b_c0_coordinator.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto projected_revalidation(HostRevalidationDisposition value)->core::Error {
  std::string classification;
  switch(value){
  case HostRevalidationDisposition::physical_fingerprint_mismatch: classification="physical_fingerprint_mismatch";break;
  case HostRevalidationDisposition::effective_fingerprint_mismatch: classification="effective_fingerprint_mismatch";break;
  case HostRevalidationDisposition::worker_cpu_unavailable: classification="worker_cpu_unavailable";break;
  case HostRevalidationDisposition::worker_node_unavailable: classification="worker_node_unavailable";break;
  case HostRevalidationDisposition::service_core_incomplete: classification="service_core_incomplete";break;
  case HostRevalidationDisposition::matched: classification="matched";break;
  }
  return {.category=core::ErrorCategory::monitoring,
    .code=core::StableErrorCode{"monitoring.q0b.host_revalidation_failed"},
    .message="Monitoring Q0b host revalidation failed",
    .operation="coordinate_host_q0b_c0_calibrations",.os_error=std::nullopt,
    .context={{"classification",classification}},.severity=core::ErrorSeverity::error,
    .continuation=core::ContinuationPolicy::abort_command};
}
auto contained(bool unknown)->core::Error {return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q0b.host_coordinator.contained_failure"},
 .message="Monitoring Q0b host coordinator failed",.operation="coordinate_host_q0b_c0_calibrations",
 .os_error=std::nullopt,.context={{"classification",unknown?"contained_unknown_failure":"contained_exception"}},
 .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};}
auto calibration_invalid()->core::Error{return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q0b.host_coordinator.invalid_calibration"},
 .message="Monitoring Q0b calibration result is invalid",
 .operation="coordinate_host_q0b_c0_calibrations",.os_error=std::nullopt,
 .context={{"classification","calibration_not_selected"}},.severity=core::ErrorSeverity::error,
 .continuation=core::ContinuationPolicy::abort_command};}
auto payload_invalid()->core::Error{return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q0b.host_coordinator.invalid_payload"},
 .message="Monitoring Q0b payload construction failed",
 .operation="coordinate_host_q0b_c0_calibrations",.os_error=std::nullopt,
 .context={{"classification","payload_invalid"}},.severity=core::ErrorSeverity::error,
 .continuation=core::ContinuationPolicy::abort_command};}
auto fail(core::Error value)->Q0bC0DriverFailure{return {std::move(value),{}};}
auto fail(runtime::CalibrationOnlyOperationFailure value)->Q0bC0DriverFailure{return {std::move(value.primary),std::move(value.secondary)};}
auto interval(const measurement::MeasurementInterval& value)->nlohmann::ordered_json{return {
 {"start_ns",value.start().nanoseconds()},{"end_ns",value.end().nanoseconds()},
 {"elapsed_ns",value.elapsed().value()}};}
template<class Outcome> auto family_json(std::string_view family,
 const MonitoringHostAdmittedProductionFacts& facts,const MonitoringHostQ0bBaseEnvironment& base,
 const Outcome& outcome)->std::expected<nlohmann::ordered_json,core::Error>{
 if(outcome.disposition()!=measurement::CalibrationDisposition::selected||!outcome.selected_candidate())
   return std::unexpected{calibration_invalid()};
 auto empty=nlohmann::ordered_json::array();for(const auto& t:outcome.empty_harness_trials())empty.push_back(interval(t.interval()));
 auto candidates=nlohmann::ordered_json::array();for(const auto& t:outcome.candidate_trials()){
   auto item=interval(t.interval());item["candidate"]=t.candidate().value();candidates.push_back(std::move(item));}
 const auto& p=outcome.duration_policy();
 return nlohmann::ordered_json{{"family",family},{"production_admission",{
   {"schema","epycbench.monitoring-admitted-production-facts/v2"},{"sha256",facts.digest().to_hex()},
   {"variant_role","first"},{"base_environment_sha256",base.digest().to_hex()}}},
   {"selected_work",outcome.selected_candidate()->value()},{"calibration",{
    {"path","RT-009 production first-variant calibration"},{"duration_policy",{
      {"minimum_duration_ns",p.minimum_duration().value()},{"target_duration_ns",p.target_duration().value()},
      {"maximum_duration_ns",p.maximum_duration().value()}}},{"empty_harness_trials",std::move(empty)},
    {"timer_overhead_estimate_ns",outcome.timer_overhead_estimate().duration().value()},
    {"candidate_trials",std::move(candidates)},{"disposition","selected"},
    {"selected_candidate",outcome.selected_candidate()->value()}}},
   {"finalization",{{"operation","CalibrationOnlyProductionRun::finalize"},{"disposition","succeeded"}}}};
}
template<class Calibrate> auto run(const runtime::CalibrationOnlyProductionOperations& operations,
 const runtime::CalibrationOnlyProductionAdmission& inputs,const measurement::CalibrationDurationPolicy& policy,
 Calibrate calibrate)->std::expected<std::decay_t<decltype(calibrate(std::declval<runtime::CalibrationOnlyProductionRun&>(),policy).value())>,Q0bC0DriverFailure>{
 auto prepared=operations.prepare_run(inputs);if(!prepared)return std::unexpected{fail(std::move(prepared.error()))};
 auto outcome=calibrate(*prepared,policy);auto finalized=std::move(*prepared).finalize();
 if(!outcome){auto result=fail(std::move(outcome.error()));if(!finalized){result.secondary.push_back(std::move(finalized.error().primary));for(auto& e:finalized.error().secondary)result.secondary.push_back(std::move(e));}return std::unexpected{std::move(result)};}
 if(!finalized)return std::unexpected{fail(std::move(finalized.error()))};
 return std::move(*outcome);
}
} // namespace

static auto run_calibrations(const RunnerContextAcquisitionSession& session,
 const runtime::CalibrationOnlyProductionOperations& operations,
 const HostQualificationCalibrationInputs& inputs,const MonitoringHostQualificationPlan& plan,
 const MonitoringHostAdmittedProductionFacts& bandwidth_facts,
 const MonitoringHostAdmittedProductionFacts& latency_facts,
 const MonitoringHostQ0bBaseEnvironment& base, HostQ0bStageAdmission admission) noexcept
 ->std::expected<Q0bC0Calibrations,Q0bC0DriverFailure>{
 try{
  auto gate=session.revalidate();if(!gate)return std::unexpected{fail(std::move(gate.error()))};
  if(*gate!=HostRevalidationDisposition::matched)return std::unexpected{fail(projected_revalidation(*gate))};
  if (admission.admit) {
    auto ready = admission.admit(admission.context, session, plan, inputs, HostQ0bFamily::bandwidth);
    if (!ready) return std::unexpected{std::move(ready.error())};
  }
  auto bandwidth=run(operations,inputs.bandwidth(),plan.calibration_policy(),[](auto& run,const auto& policy){return run.calibrate_bandwidth(policy);});
  if(!bandwidth)return std::unexpected{std::move(bandwidth.error())};
  gate=session.revalidate();if(!gate)return std::unexpected{fail(std::move(gate.error()))};
  if(*gate!=HostRevalidationDisposition::matched)return std::unexpected{fail(projected_revalidation(*gate))};
  if (admission.admit) {
    auto ready = admission.admit(admission.context, session, plan, inputs, HostQ0bFamily::latency);
    if (!ready) return std::unexpected{std::move(ready.error())};
  }
  auto latency=run(operations,inputs.latency(),plan.calibration_policy(),[](auto& run,const auto& policy){return run.calibrate_latency(policy);});
  if(!latency)return std::unexpected{std::move(latency.error())};
  auto bw=family_json("sequential-read-bandwidth",bandwidth_facts,base,*bandwidth);
  auto lat=family_json("dependent-load-latency",latency_facts,base,*latency);
  if(!bw)return std::unexpected{fail(std::move(bw.error()))};
  if(!lat)return std::unexpected{fail(std::move(lat.error()))};
  auto families=nlohmann::ordered_json::array();families.push_back(std::move(*bw));families.push_back(std::move(*lat));
  return detail_HostQ0bC0CoordinatorAccess::make(std::move(families),inputs.bandwidth().service_placement().assignments()[0]);
 }catch(const std::exception&){return std::unexpected{fail(contained(false))};}
 catch(...){return std::unexpected{fail(contained(true))};}
}

auto coordinate_host_q0b_c0_calibrations(std::string_view qualification_id,
 std::string_view collected_at,const HostEnvironmentAuthority& environment,
 const RunnerContextAcquisitionSession& session,
 const runtime::CalibrationOnlyProductionOperations& operations,
 const HostQualificationCalibrationInputs& inputs,const MonitoringHostQualificationPlan& plan,
 const MonitoringHostAdmittedProductionFacts& bandwidth_facts,
 const MonitoringHostAdmittedProductionFacts& latency_facts,
 const MonitoringHostQ0bBaseEnvironment& base, HostQ0bStageAdmission admission) noexcept
 ->std::expected<MonitoringHostQ0bBackendResult,Q0bC0DriverFailure>{
 try{
  auto calibrations=run_calibrations(session,operations,inputs,plan,bandwidth_facts,latency_facts,base,admission);
  if(!calibrations)return std::unexpected{std::move(calibrations.error())};
  const auto fault=detail_HostQ0bV2TestAccess::fault();
  if(fault==detail_HostQ0bV2TestAccess::Fault::payload_standard_exception)
    throw std::runtime_error{"private"};
  if(fault==detail_HostQ0bV2TestAccess::Fault::payload_unknown_exception)throw 7;
  if(fault==detail_HostQ0bV2TestAccess::Fault::payload_invalid)
    return std::unexpected{fail(payload_invalid())};
  auto characterization=build_monitoring_host_q0b_characterization(qualification_id,collected_at,
    session,environment,plan,base,bandwidth_facts,latency_facts,*calibrations);
  if(!characterization)return std::unexpected{fail(std::move(characterization.error()))};
  auto result=build_monitoring_host_q0b_backend_result(environment,*characterization);
  if(!result)return std::unexpected{fail(std::move(result.error()))};
  return std::move(*result);
 }catch(const std::exception&){return std::unexpected{fail(contained(false))};}
 catch(...){return std::unexpected{fail(contained(true))};}
}
} // namespace epycbench::monitoring::qualification::detail
