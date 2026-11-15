#include "monitoring/host_q2_backend.hpp"
#include "monitoring/host_q2_backend_test_access.hpp"
#include "monitoring/host_q1_backend.hpp"
#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_context_switch_collector.hpp"
#include "monitoring/host_q2_context_switch_evidence.hpp"
#include "monitoring/host_q2_terminal_containment.hpp"
#include "monitoring/host_running_build_attestation_value.hpp"
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/monitoring/host_environment.hpp"
#include "epycbench/monitoring/host_input_template.hpp"
#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "monitoring/host_qualification_composition.hpp"
#include "monitoring/privacy_validation.hpp"
#include "runtime/controller_process_production_authority.hpp"
#include <array>
#include <algorithm>
#include <optional>
namespace epycbench::monitoring::qualification::detail {
namespace {
using Json=nlohmann::ordered_json;
using BackendPhase=MonitoringHostQ2BackendPhase;
auto phase_token(BackendPhase value) noexcept->std::string_view {switch(value){
 case BackendPhase::repository:return "repository";case BackendPhase::collection_plan:return "collection_plan";
 case BackendPhase::predecessor:return "predecessor";case BackendPhase::build_attestation:return "build_attestation";
 case BackendPhase::permission:return "permission";case BackendPhase::session:return "session";
 case BackendPhase::worker_admission:return "worker_admission";case BackendPhase::source_preparation:return "source_preparation";
 case BackendPhase::voluntary_event:return "voluntary_event";case BackendPhase::preemption_event:return "preemption_event";
 case BackendPhase::drain:return "drain";case BackendPhase::association:return "association";
 case BackendPhase::clock:return "clock";case BackendPhase::completeness:return "completeness";
 case BackendPhase::finalization:return "finalization";case BackendPhase::evidence_construction:return "evidence_construction";
 case BackendPhase::usage:return "usage";}
 std::terminate();}
constexpr auto phase_index(BackendPhase value) noexcept->std::size_t {
 return static_cast<std::size_t>(value);
}
auto error(std::string classification, core::ErrorCategory category=core::ErrorCategory::monitoring,
           std::string code="monitoring.q2.backend.stage_failure") -> core::Error {
 return {.category=category,.code=core::StableErrorCode{std::move(code)},
  .message="Monitoring Q2 backend failed",.operation="run_monitoring_host_q2_backend",
  .os_error=std::nullopt,.context={{"classification",std::move(classification)}},
  .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};
}
auto staged(std::string_view name,const core::Error* underlying=nullptr)->core::Error {
 auto out=error(std::string{name});
 out.context=nullptr;
 if(underlying)out.os_error=underlying->os_error;
 return out;
}
class FailureStock final {
 public:
  FailureStock(){for(std::size_t index{};index<values_.size();++index)
    {values_[index]=staged(phase_token(static_cast<BackendPhase>(index)));
     values_[index].message.clear();}}
  auto take(BackendPhase phase,const core::Error* underlying=nullptr) noexcept->core::Error {
    const auto index=phase_index(phase);if(taken_[index])std::terminate();taken_[index]=true;
    auto result=std::move(values_[index]);
    if(underlying&&underlying->os_error){result.os_error.emplace();result.os_error->value=underlying->os_error->value;}
    return result;
  }
 private:
  std::array<core::Error,16> values_{};std::array<bool,16> taken_{};
};
auto bootstrap_failure(BackendPhase phase) noexcept
 ->std::expected<MonitoringHostQ2BackendResult,MonitoringHostQ2BackendFailure>{
 MonitoringHostQ2BackendFailure out{{.category=core::ErrorCategory::monitoring,
  .code=core::StableErrorCode{},.message={},.operation={},.os_error=std::nullopt,.context=nullptr,
  .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command},{}};
 out.phase=phase;
 return std::unexpected{std::move(out)};
}
void retain(MonitoringHostQ2BackendFailure& target,core::Error value) noexcept {
 if(target.secondary_count<target.secondary.size())target.secondary[target.secondary_count++].emplace(std::move(value));
 else target.cleanup_overflowed=true;
}
void retain_underlying(MonitoringHostQ2BackendFailure& target,const core::Error* value) noexcept{
 if(!value)return;
 const auto& code=value->code.value();if(code.empty()||code.size()>target.underlying_code.size())return;
 std::copy(code.begin(),code.end(),target.underlying_code.begin());target.underlying_code_size=code.size();
}
auto repository_classification(std::string_view value) noexcept->bool {
 constexpr std::array allowed{"repository_authority","git_executable","git_identity","git_metadata","git_config",
  "git_result","git_poll","git_read","git_output_limit","git_setup","git_exit",
  "git_spawn","repository_dirty","tracked_entry"};
 return std::ranges::find(allowed,value)!=allowed.end();
}
void retain_repository_classification(MonitoringHostQ2BackendFailure& target,
                                      const core::Error& value) noexcept {
 if(value.code.value()!="monitoring.q1_plan_authority.repository_rejected"||
    !value.context.is_object())return;
 auto found=value.context.find("classification");
 if(found==value.context.end()||!found->is_string())return;
 const auto& classification=found->get_ref<const std::string&>();
 if(!repository_classification(classification)||classification.size()>target.repository_classification.size())return;
 std::copy(classification.begin(),classification.end(),target.repository_classification.begin());
 target.repository_classification_size=classification.size();
}
auto authority_backend_phase(MonitoringQ2AuthorityStage value) noexcept->BackendPhase {
 switch(value){
 case MonitoringQ2AuthorityStage::repository:return BackendPhase::repository;
 case MonitoringQ2AuthorityStage::collection_plan:return BackendPhase::collection_plan;
 case MonitoringQ2AuthorityStage::predecessor:return BackendPhase::predecessor;
 case MonitoringQ2AuthorityStage::build_attestation:return BackendPhase::build_attestation;
 case MonitoringQ2AuthorityStage::worker_admission:return BackendPhase::worker_admission;
 }
 std::terminate();
}
auto authority_failure(MonitoringQ2AuthorityFailure value,FailureStock& stock)
 ->std::expected<MonitoringHostQ2BackendResult,MonitoringHostQ2BackendFailure>{
 const auto selected=authority_backend_phase(value.stage);
 MonitoringHostQ2BackendFailure out{stock.take(selected,&value.primary),{}};
 out.phase=selected;
 retain_underlying(out,&value.primary);
 if(selected==BackendPhase::repository)retain_repository_classification(out,value.primary);
 for(std::size_t i{};i<value.cleanup_count;++i) retain(out,std::move(*value.cleanup[i]));
 out.cleanup_overflowed=value.cleanup_overflowed;
 return std::unexpected{std::move(out)};
}
auto collection_backend_phase(MonitoringQ2CollectionStage stage) noexcept -> BackendPhase {
 switch(stage){
 case MonitoringQ2CollectionStage::preparation:
 case MonitoringQ2CollectionStage::session:return BackendPhase::session;
 case MonitoringQ2CollectionStage::source_preparation:return BackendPhase::source_preparation;
 case MonitoringQ2CollectionStage::voluntary_event:return BackendPhase::voluntary_event;
 case MonitoringQ2CollectionStage::preemption_event:return BackendPhase::preemption_event;
 case MonitoringQ2CollectionStage::drain:return BackendPhase::drain;
 case MonitoringQ2CollectionStage::association:return BackendPhase::association;
 case MonitoringQ2CollectionStage::clock:return BackendPhase::clock;
 case MonitoringQ2CollectionStage::completeness:return BackendPhase::completeness;
 case MonitoringQ2CollectionStage::finalization:return BackendPhase::finalization;
 }
 std::terminate();
}

struct ProductionBackendState final {
 std::optional<MonitoringQ2AuthoritySession> authority;
 std::optional<MonitoringQ2CollectorPlan> collector;
 std::optional<MonitoringQ2ContextSwitchCollectionOutcome> outcome;
 std::optional<RunnerContextAcquisitionSession> session;
 std::optional<MonitoringHostQualificationPlan> plan;
 std::optional<HostQualificationCalibrationInputs> inputs;
 runtime::ControllerProcessProductionAuthority* controller{};
 Json build;
 std::filesystem::path root;
 std::string_view qid;
};

auto production_authenticate(void* opaque,const std::filesystem::path& root,std::string_view qid)
 ->std::expected<void,MonitoringQ2AuthorityFailure>{
 auto& state=*static_cast<ProductionBackendState*>(opaque);state.root=root;state.qid=qid;
 auto value=MonitoringQ2AuthoritySession::authenticate(root,qid);
 if(!value)return std::unexpected{std::move(value.error())};
 state.authority.emplace(std::move(*value));return {};
}
auto production_validate_build(void* opaque)->std::expected<void,core::Error>{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 auto parsed=Json::parse(state.authority->validated_build_attestation());
 auto value=attest_monitoring_running_build_value(parsed,state.authority->authenticated_repository_commit());
 if(!value)return std::unexpected{value.error()};
 state.build=std::move(*value);return {};
}
auto production_acquire_session(void* opaque,runtime::ControllerProcessProductionAuthority& controller)
 ->std::expected<void,core::Error>{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 state.controller=&controller;
 auto input=load_monitoring_host_input_template(state.root);if(!input)return std::unexpected{input.error()};
 auto session=acquire_runner_context_session(std::move(*input));if(!session)return std::unexpected{session.error()};
 state.session.emplace(std::move(*session));
 auto environment=derive_host_environment(*state.session);if(!environment)return std::unexpected{environment.error()};
 auto plan=resolve_monitoring_host_qualification_plan(*state.session,*environment);if(!plan)return std::unexpected{plan.error()};
 state.plan.emplace(std::move(*plan));
 return {};
}
auto production_admit_workers(void* opaque)->std::expected<void,core::Error>{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 auto context=state.session->runner_context();if(!context)return std::unexpected{context.error()};
 runtime::PairwiseProductionCompositionContext composition{context->get(),state.controller->request_ids()};
 auto inputs=compose_host_qualification_calibration_inputs(*state.plan,*state.session,composition);
 if(!inputs)return std::unexpected{inputs.error()};
 state.inputs.emplace(std::move(*inputs));return {};
}
auto production_bind(void* opaque)->std::expected<void,MonitoringQ2AuthorityFailure>{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 auto value=state.authority->bind_fresh_plan(*state.session,*state.plan,state.inputs->bandwidth().first());
 if(!value)return std::unexpected{std::move(value.error())};
 state.collector.emplace(std::move(*value));
 state.authority.reset();return {};
}
auto production_collect(void* opaque)->MonitoringHostQ2BackendOperations::CollectionDisposition{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 state.outcome.emplace(MonitoringQ2ContextSwitchCollector::collect(std::move(*state.collector)));
 if(std::holds_alternative<MonitoringQ2TerminalContainment>(*state.outcome))
  return MonitoringHostQ2BackendOperations::CollectionDisposition::terminal;
 if(std::holds_alternative<MonitoringQ2FailedCollection>(*state.outcome))
  return MonitoringHostQ2BackendOperations::CollectionDisposition::failed;
 return MonitoringHostQ2BackendOperations::CollectionDisposition::evidence;
}
auto production_take_collection_failure(void* opaque) noexcept->MonitoringQ2FailedCollection{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 return std::move(std::get<MonitoringQ2FailedCollection>(*state.outcome));
}
auto production_construct_evidence(void* opaque)
 ->std::expected<MonitoringHostQ2BackendResult,core::Error>{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 auto evidence=construct_monitoring_q2_context_switch_evidence(std::move(*state.outcome));
 if(!evidence)return std::unexpected{evidence.error()};
 auto evidence_json=Json::parse(evidence->canonical_json);
 Json wrapper{{"schema","epycbench.monitoring-host-q2-backend-result/v1"},
              {"build_manifest",std::move(state.build)},{"evidence",std::move(evidence_json)}};
 if(!privacy_safe(wrapper))return std::unexpected{error("evidence_construction")};
 auto bytes=core::canonicalize_json(wrapper);
 if(!bytes||bytes->size()+1U>20971520U)return std::unexpected{error("evidence_construction")};
 bytes->push_back('\n');
 if(!validate_monitoring_host_q2_backend_success(*bytes))return std::unexpected{error("evidence_construction")};
 return MonitoringHostQ2BackendResult{MonitoringHostQ2BackendBytes{std::move(*bytes)}};
}
auto production_retire(void* opaque) noexcept->MonitoringQ2AuthoritySession::PrebindRetirement{
 auto& state=*static_cast<ProductionBackendState*>(opaque);auto result=state.authority->retire_before_binding();
 state.authority.reset();return result;
}
[[noreturn]] void production_consume_terminal(void* opaque) noexcept{
 auto& state=*static_cast<ProductionBackendState*>(opaque);
 consume_monitoring_q2_terminal_containment(
  std::move(std::get<MonitoringQ2TerminalContainment>(*state.outcome)));
}

auto run_backend_body(const std::filesystem::path& root,std::string_view qid,
 runtime::ControllerProcessProductionAuthority& controller,const MonitoringHostQ2BackendOperations& operations) noexcept
 ->std::expected<MonitoringHostQ2BackendResult,MonitoringHostQ2BackendFailure>{
 BackendPhase active_stage=BackendPhase::repository;bool owns_authority=false;std::optional<FailureStock> stock;
 const auto finish_failure=[&](BackendPhase phase,const core::Error* underlying=nullptr){
  MonitoringHostQ2BackendFailure out{stock->take(phase,underlying),{}};out.phase=phase;
  retain_underlying(out,underlying);
  if(owns_authority){auto cleanup=operations.retire(operations.context);owns_authority=false;
   for(std::size_t i{};i<cleanup.count;++i)retain(out,std::move(*cleanup.errors[i]));
   out.cleanup_overflowed=cleanup.overflowed;}
  return std::expected<MonitoringHostQ2BackendResult,MonitoringHostQ2BackendFailure>{std::unexpected{std::move(out)}};
 };
 try{
  stock.emplace();
  auto authenticated=operations.authenticate(operations.context,root,qid);
  if(!authenticated)return authority_failure(std::move(authenticated.error()),*stock);
  owns_authority=true;
  active_stage=BackendPhase::build_attestation;auto build=operations.validate_build(operations.context);
  if(!build)return finish_failure(active_stage,&build.error());
  active_stage=BackendPhase::session;auto session=operations.acquire_session(operations.context,controller);
  if(!session)return finish_failure(active_stage,&session.error());
  active_stage=BackendPhase::worker_admission;auto admission=operations.admit_workers(operations.context);
  if(!admission)return finish_failure(active_stage,&admission.error());
  auto bound=operations.bind(operations.context);
  if(!bound){owns_authority=false;return authority_failure(std::move(bound.error()),*stock);}
  owns_authority=false;
  active_stage=BackendPhase::session;
  const auto disposition=operations.collect(operations.context);
  if(disposition==MonitoringHostQ2BackendOperations::CollectionDisposition::terminal)
   operations.consume_terminal(operations.context);
  if(disposition==MonitoringHostQ2BackendOperations::CollectionDisposition::failed){
   auto failure=operations.take_collection_failure(operations.context);
   const auto selected=collection_backend_phase(failure.stage);auto out=MonitoringHostQ2BackendFailure{stock->take(selected,&failure.primary),{}};out.phase=selected;
   retain_underlying(out,&failure.primary);
   if(selected==BackendPhase::completeness&&
      failure.primary.code.value()=="monitoring.q2.collector.required_event")
    out.required_event=failure.required_event;
   for(std::size_t i{};i<failure.cleanup_count;++i)retain(out,std::move(*failure.cleanup[i]));
   if(failure.cleanup_overflowed)out.cleanup_overflowed=true;
   return std::unexpected{std::move(out)};
  }
  active_stage=BackendPhase::evidence_construction;auto evidence=operations.construct_evidence(operations.context);
  if(!evidence)return finish_failure(active_stage,&evidence.error());
  return std::move(*evidence);
 }catch(...){if(stock)return finish_failure(active_stage);return bootstrap_failure(active_stage);}
}
}
auto monitoring_q2_backend_phase(MonitoringQ2CollectionStage stage) noexcept -> BackendPhase {
 return collection_backend_phase(stage);
}
auto monitoring_q2_backend_phase(MonitoringQ2AuthorityStage stage) noexcept -> BackendPhase {
 return authority_backend_phase(stage);
}
auto monitoring_host_q2_usage_failure() noexcept->MonitoringHostQ2BackendFailure {
 auto out=MonitoringHostQ2BackendFailure{{.category=core::ErrorCategory::usage,
  .code=core::StableErrorCode{"monitoring.q2.backend.usage"},
  .message="Invalid hidden Monitoring Q2 arguments",.operation="run_monitoring_host_q2_backend",
  .os_error=std::nullopt,.context=nlohmann::json::object(),.severity=core::ErrorSeverity::error,
  .continuation=core::ContinuationPolicy::abort_command},{}};out.phase=BackendPhase::usage;return out;
}
auto validate_monitoring_host_q2_backend_success(std::string_view bytes) noexcept -> bool {
 try {
  if(bytes.empty()||bytes.back()!='\n'||bytes.size()>20U*1024U*1024U)return false;
  const auto body=bytes.substr(0,bytes.size()-1U);auto value=Json::parse(body);
  if(!value.is_object()||value.size()!=3U||value.value("schema","")!="epycbench.monitoring-host-q2-backend-result/v1"||
     !value.contains("build_manifest")||!value.contains("evidence"))return false;
  auto canonical=core::canonicalize_json(value);if(!canonical||*canonical!=body)return false;
  auto evidence_bytes=core::canonicalize_json(value.at("evidence"));
  if(!evidence_bytes||evidence_bytes->size()+1U>16U*1024U*1024U)return false;
  evidence_bytes->push_back('\n');
  if(!validate_monitoring_q2_context_switch_evidence(*evidence_bytes))return false;
  const auto& build=value.at("build_manifest");const auto& evidence=value.at("evidence");
  // The retained v1 artifact is historical input for Q3/Q4, never a current Q2 backend result.
  if(evidence.value("schema","")!="epycbench.monitoring-q2-context-switch-evidence/v2")return false;
  if(!build.is_object()||!build.contains("git")||!build.contains("build_identity")||
     !build.at("git").is_object()||!build.at("git").contains("revision")||
     !evidence.contains("provenance")||!evidence.at("provenance").is_object())return false;
  const auto& provenance=evidence.at("provenance");const auto commit=provenance.value("repository_commit","");
  auto build_bytes=core::canonicalize_json(build);if(!build_bytes||build_bytes->size()+1U>1024U*1024U)return false;
  auto validated_build=attest_monitoring_running_build_value(build,commit);if(!validated_build)return false;
  auto validated_bytes=core::canonicalize_json(*validated_build);
  return validated_bytes&&*validated_bytes==*build_bytes&&commit==build.at("git").value("revision","")&&
         provenance.value("build_identity","")==std::string{"sha256:"}+build.value("build_identity","");
 }catch(...){return false;}
}
auto run_monitoring_host_q2_backend(const std::filesystem::path& root,std::string_view qid,
 runtime::ControllerProcessProductionAuthority& controller) noexcept
 ->std::expected<MonitoringHostQ2BackendResult,MonitoringHostQ2BackendFailure>{
 ProductionBackendState state;MonitoringHostQ2BackendOperations operations{
  .context=&state,.authenticate=production_authenticate,.validate_build=production_validate_build,
  .acquire_session=production_acquire_session,.admit_workers=production_admit_workers,
  .bind=production_bind,.collect=production_collect,.take_collection_failure=production_take_collection_failure,
  .construct_evidence=production_construct_evidence,
  .retire=production_retire,.consume_terminal=production_consume_terminal};
 return run_backend_body(root,qid,controller,operations);
}
auto MonitoringHostQ2BackendTestAccess::run(const std::filesystem::path& root,std::string_view qid,
 runtime::ControllerProcessProductionAuthority& controller,const MonitoringHostQ2BackendOperations& operations) noexcept
 ->std::expected<MonitoringHostQ2BackendResult,MonitoringHostQ2BackendFailure>{
 return run_backend_body(root,qid,controller,operations);
}
auto project_monitoring_host_q2_authored(std::span<const scenario::ScenarioDiagnostic> diagnostics) noexcept
 ->std::expected<Json,core::Error>{auto projected=project_monitoring_host_q1_authored(diagnostics);
 if(!projected) return std::unexpected{projected.error()};
 (*projected)["schema"]="epycbench.monitoring-host-q2-backend-authored/v1";
 return *projected;}
}
