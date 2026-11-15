#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"
#include "runtime/monitoring_q1_worker_session_test_access.hpp"

namespace epycbench::runtime::detail {
namespace {
thread_local MonitoringQ1WorkerSessionTestAccess::ProjectionFault projection_fault{};
auto session_error(std::string classification)->core::Error{return {
 .category=core::ErrorCategory::runtime,
 .code=core::StableErrorCode{"runtime.monitoring_q1_worker_session.finalization_failed"},
 .message="Monitoring Q1 worker session finalization failed",
 .operation="monitoring_q1_worker_session_finalize",.os_error=std::nullopt,
 .context={{"classification",std::move(classification)}},.severity=core::ErrorSeverity::error,
 .continuation=core::ContinuationPolicy::abort_command};}
auto session_invariant(std::string classification)->core::Error{auto value=session_error(std::move(classification));
 value.code=core::StableErrorCode{"runtime.monitoring_q1_worker_session.invariant_failed"};return value;}
}

struct MonitoringQ1WorkerSession::State {
 MonitoringQ1AdmittedWorkerSnapshot admitted;
 TargetedGenerationControl control;
 std::optional<PersistentWorkerTeam> team;
 std::vector<core::Error> secondary;
 bool test_faults{};
 core::Error projection_bad_alloc;
 core::Error projection_standard;
 core::Error projection_unknown;
 core::Error stop_standard;
 core::Error stop_unknown;
 std::size_t secondary_capacity{};
 State(MonitoringQ1AdmittedWorkerSnapshot value,TargetedGenerationControl c,
       std::vector<core::Error> storage,core::Error bad,core::Error standard,
       core::Error unknown,core::Error stop_std,core::Error stop_unk,
       std::size_t max_secondary) noexcept
  :admitted(std::move(value)),control(std::move(c)),secondary(std::move(storage)),
   projection_bad_alloc(std::move(bad)),projection_standard(std::move(standard)),
   projection_unknown(std::move(unknown)),stop_standard(std::move(stop_std)),
   stop_unknown(std::move(stop_unk)),secondary_capacity(max_secondary){}
};

MonitoringQ1WorkerSessionFinalization::MonitoringQ1WorkerSessionFinalization(
 MonitoringQ1WorkerSessionDisposition d,std::optional<core::Error> p,
 std::vector<core::Error> s) noexcept:disposition_(d),primary_(std::move(p)),secondary_(std::move(s)){}
MonitoringQ1WorkerSession::MonitoringQ1WorkerSession(std::unique_ptr<State> s) noexcept:state_(std::move(s)){}
MonitoringQ1WorkerSession::MonitoringQ1WorkerSession(MonitoringQ1WorkerSession&&) noexcept=default;
MonitoringQ1WorkerSession::~MonitoringQ1WorkerSession() noexcept{
 if(!state_)return;
 const bool borrowed=PersistentWorkerTeamAccess::targeted_control_borrowed(state_->control);
 auto stopped=PersistentWorkerTeamAccess::stop_monitoring_q1_session_noexcept(
   *state_->team,std::move(state_->stop_standard),std::move(state_->stop_unknown));
 if(!stopped&&state_->team->active()){
  const auto forced=PersistentWorkerTeamAccess::force_stop_and_join_after_failure(*state_->team);
  if(forced.disposition==ForcedTeamStopDisposition::active)std::terminate();
 }
 if(state_->test_faults)
  PersistentWorkerTeamAccess::clear_stop_failures_for_test(*state_->team);
 if(borrowed)std::terminate();
}

auto project_monitoring_q1_worker_snapshot(const AdmittedPlacementRequest& admitted) noexcept
 ->std::expected<MonitoringQ1AdmittedWorkerSnapshot,core::Error>{
 auto projected=PersistentWorkerTeamAccess::project(admitted.workers());
 if(!projected)return std::unexpected{std::move(projected.error())};
 if(projected->empty())std::terminate();
 return MonitoringQ1AdmittedWorkerSnapshot{std::move(*projected)};
}

auto make_monitoring_q1_worker_session(MonitoringQ1AdmittedWorkerSnapshot&& admitted,RunnerCancellation& cancellation,
 WorkerTeamDependencies dependencies)->std::expected<MonitoringQ1WorkerSession,core::Error>{
 auto contained_bad_alloc=session_error("contained_standard_exception");
 auto contained_standard=session_error("contained_standard_exception");
 auto contained_unknown=session_error("contained_unknown_exception");
 try{
  if(admitted.records_.size()>(std::numeric_limits<std::size_t>::max()-3U)/2U)
   return std::unexpected{session_error("force_capacity_exhausted")};
  const auto capacity=admitted.records_.size()*2U+3U;
  std::vector<core::Error> storage;storage.reserve(capacity);
  auto projection_bad=session_error("projection_bad_alloc");
  auto projection_standard=session_error("projection_standard_exception");
  auto projection_unknown=session_error("projection_unknown_exception");
  auto stop_standard=session_error("contained_standard_exception");
  auto stop_unknown=session_error("contained_unknown_exception");
  auto control=make_monitoring_q1_targeted_control();if(!control)return std::unexpected{control.error()};
  auto state=std::make_unique<MonitoringQ1WorkerSession::State>(
    std::move(admitted),std::move(*control),std::move(storage),
    std::move(projection_bad),std::move(projection_standard),std::move(projection_unknown),
    std::move(stop_standard),std::move(stop_unknown),
    capacity);
  auto team=PersistentWorkerTeamAccess::create_from_launch_specs(
    std::vector<WorkerLaunchSpec>{state->admitted.records_},cancellation,std::move(dependencies));
  if(!team)return std::unexpected{team.error()};
  state->team.emplace(std::move(*team));
  return MonitoringQ1WorkerSession{std::move(state)};
 }catch(const std::bad_alloc&){return std::unexpected{std::move(contained_bad_alloc)};}
 catch(const std::exception&){return std::unexpected{std::move(contained_standard)};}
 catch(...){return std::unexpected{std::move(contained_unknown)};}
}
auto create_monitoring_q1_worker_session(const AdmittedPlacementRequest& admitted,
 RunnerCancellation& cancellation)->std::expected<MonitoringQ1WorkerSession,core::Error>{
 auto snapshot=project_monitoring_q1_worker_snapshot(admitted);
 if(!snapshot)return std::unexpected{std::move(snapshot.error())};
 return make_monitoring_q1_worker_session(std::move(*snapshot),cancellation,production_worker_team_dependencies());
}
auto make_monitoring_q2_worker_session(MonitoringQ1AdmittedWorkerSnapshot&& snapshot,
 RunnerCancellation& cancellation)->std::expected<MonitoringQ1WorkerSession,core::Error>{
 return make_monitoring_q1_worker_session(std::move(snapshot),cancellation,
                                          production_worker_team_dependencies());
}
auto monitoring_q2_worker_readiness(const MonitoringQ1WorkerSession& session,std::size_t index) noexcept
 ->std::expected<WorkerReadyEvidence,core::Error>{
 if(!session.state_)std::terminate();
 const auto ready=session.state_->team->readiness();
 if(index>=ready.size())return std::unexpected{session_error("q2_worker_index")};
 return ready[index];
}
auto MonitoringQ1WorkerSessionTestAccess::create(const AdmittedPlacementRequest& admitted,
 RunnerCancellation& cancellation,WorkerTeamDependencies dependencies)
 ->std::expected<MonitoringQ1WorkerSession,core::Error>{
 auto snapshot=project_monitoring_q1_worker_snapshot(admitted);
 if(!snapshot)return std::unexpected{std::move(snapshot.error())};
 return make_monitoring_q1_worker_session(std::move(*snapshot),cancellation,std::move(dependencies));
}
auto begin_monitoring_q1_targeted_generation(MonitoringQ1WorkerSession& session,std::size_t index,
 TargetedWorkerOperation operation,WorkerCancellationPolicy cancellation)
 ->std::expected<TargetedWorkerGeneration,core::Error>{
 if(!session.state_)std::terminate();
 return PersistentWorkerTeamAccess::begin_targeted_generation_preallocated(
   session.state_->admitted.records_,*session.state_->team,session.state_->control,index,
   operation,cancellation);
}
auto begin_monitoring_q2_targeted_generation(MonitoringQ1WorkerSession& session,std::size_t index,
 TargetedWorkerOperation operation,TargetedWorkerReadinessOperation readiness,
 WorkerCancellationPolicy cancellation)->std::expected<TargetedWorkerGeneration,core::Error>{
 if(!session.state_)std::terminate();
 return PersistentWorkerTeamAccess::begin_q2_targeted_generation_preallocated(
   session.state_->admitted.records_,*session.state_->team,session.state_->control,index,
   operation,readiness,cancellation);
}
auto begin_monitoring_q2_targeted_generation(const AdmittedPlacementRequest& admitted,
 PersistentWorkerTeam& team,TargetedGenerationControl& control,std::size_t index,
 TargetedWorkerOperation operation,TargetedWorkerReadinessOperation readiness,
 WorkerCancellationPolicy cancellation)->std::expected<TargetedWorkerGeneration,core::Error>{
 auto projected=PersistentWorkerTeamAccess::project(admitted.workers());
 if(!projected)return std::unexpected{std::move(projected.error())};
 return PersistentWorkerTeamAccess::begin_q2_targeted_generation_preallocated(
   *projected,team,control,index,operation,readiness,cancellation);
}
auto reset_monitoring_q1_targeted_control(MonitoringQ1WorkerSession& session) noexcept
 ->std::expected<void,core::Error>{
 if(!session.state_)std::terminate();
 return reset_monitoring_q1_targeted_control(session.state_->control);
}
auto finalize_monitoring_q1_worker_session(MonitoringQ1WorkerSession&& session) noexcept
 ->MonitoringQ1WorkerSessionFinalization{
 if(!session.state_)return {MonitoringQ1WorkerSessionDisposition::session_moved,std::nullopt,{}};
 if(PersistentWorkerTeamAccess::targeted_control_borrowed(session.state_->control))
  return {MonitoringQ1WorkerSessionDisposition::active_generation,std::nullopt,{}};
 auto state=std::move(session.state_);std::optional<core::Error> primary;
 auto stopped=PersistentWorkerTeamAccess::stop_monitoring_q1_session_noexcept(
   *state->team,std::move(state->stop_standard),std::move(state->stop_unknown));
 if(!stopped)primary.emplace(std::move(stopped.error()));
 if(primary&&state->team->active()){
  const auto forced=PersistentWorkerTeamAccess::force_stop_and_join_after_failure(*state->team);
  if(forced.disposition==ForcedTeamStopDisposition::active)std::terminate();
  try{
   for(const auto& item:forced.errors){
    const auto fault=std::exchange(
      projection_fault,MonitoringQ1WorkerSessionTestAccess::ProjectionFault::none);
    if(fault==MonitoringQ1WorkerSessionTestAccess::ProjectionFault::bad_alloc)throw std::bad_alloc{};
    if(fault==MonitoringQ1WorkerSessionTestAccess::ProjectionFault::standard_exception)throw std::runtime_error{"projection"};
    if(fault==MonitoringQ1WorkerSessionTestAccess::ProjectionFault::unknown_exception)throw 7;
    std::string_view classification;
    switch(item.kind){
     case ForcedTeamStopErrorKind::join_failed:classification="forced_join_failed";break;
     case ForcedTeamStopErrorKind::contained_exception:classification="contained_standard_exception";break;
     case ForcedTeamStopErrorKind::contained_unknown_failure:classification="contained_unknown_exception";break;
     case ForcedTeamStopErrorKind::capacity_exhausted:
      if(state->secondary.size()<state->secondary_capacity)
       state->secondary.push_back(session_invariant("force_capacity_exhausted"));
      else if(!state->secondary.empty())
       state->secondary.back()=session_invariant("force_capacity_exhausted");
      continue;
    }
    if(state->secondary.size()<state->secondary_capacity)
     state->secondary.push_back(session_error(std::string{classification}));
    else if(!state->secondary.empty() &&
            state->secondary.back().context.value("classification","")!="force_capacity_exhausted")
     state->secondary.back()=session_invariant("force_capacity_exhausted");
   }
  }catch(const std::bad_alloc&){
   if(state->secondary.size()<state->secondary_capacity)state->secondary.push_back(
    std::move(state->projection_bad_alloc));
   else state->secondary.back()=std::move(state->projection_bad_alloc);
  }catch(const std::exception&){
   if(state->secondary.size()<state->secondary_capacity)state->secondary.push_back(
    std::move(state->projection_standard));
   else state->secondary.back()=std::move(state->projection_standard);
  }catch(...){
   if(state->secondary.size()<state->secondary_capacity)state->secondary.push_back(
    std::move(state->projection_unknown));
   else state->secondary.back()=std::move(state->projection_unknown);
  }
 }
 if(state->test_faults)PersistentWorkerTeamAccess::clear_stop_failures_for_test(*state->team);
 return {primary?MonitoringQ1WorkerSessionDisposition::failed:
                 MonitoringQ1WorkerSessionDisposition::succeeded,
   std::move(primary),std::move(state->secondary)};
}
void MonitoringQ1WorkerSessionTestAccess::inject_stop_failure(
 MonitoringQ1WorkerSession& session,std::uint32_t worker) noexcept{
 PersistentWorkerTeamAccess::inject_stop_failure_for_test(*session.state_->team,worker);
 session.state_->test_faults=true;
}
void MonitoringQ1WorkerSessionTestAccess::inject_force_failure(
 MonitoringQ1WorkerSession& session,std::uint8_t sweep,std::uint32_t worker,
 ForcedTeamStopTestFault fault) noexcept{
 PersistentWorkerTeamAccess::inject_force_stop_failure_for_test(
  *session.state_->team,sweep,worker,fault);
 session.state_->test_faults=true;
}
void MonitoringQ1WorkerSessionTestAccess::set_projection_fault(ProjectionFault fault) noexcept{
 projection_fault=fault;
}
void MonitoringQ1WorkerSessionTestAccess::set_force_capacity(
 MonitoringQ1WorkerSession& session,std::size_t capacity) noexcept{
 PersistentWorkerTeamAccess::set_force_error_capacity_for_test(*session.state_->team,capacity);
 session.state_->test_faults=true;
}
void MonitoringQ1WorkerSessionTestAccess::inject_inactive_stop_failure(
 MonitoringQ1WorkerSession& session) noexcept{
 PersistentWorkerTeamAccess::inject_inactive_stop_failure_for_test(*session.state_->team);
 session.state_->test_faults=true;
}
void MonitoringQ1WorkerSessionTestAccess::inject_eligibility_failure(
 MonitoringQ1WorkerSession& session,StopEligibilityTestFault fault) noexcept{
 PersistentWorkerTeamAccess::inject_stop_eligibility_failure_for_test(*session.state_->team,fault);
 session.state_->test_faults=true;
}
void MonitoringQ1WorkerSessionTestAccess::inject_stop_exception(
 MonitoringQ1WorkerSession& session,MonitoringQ1StopExceptionTestFault fault) noexcept{
 PersistentWorkerTeamAccess::inject_monitoring_q1_stop_exception_for_test(
  *session.state_->team,fault);
 session.state_->test_faults=true;
}

auto make_monitoring_q1_targeted_control()
    -> std::expected<TargetedGenerationControl, core::Error> {
    return PersistentWorkerTeamAccess::make_targeted_control();
}

auto begin_monitoring_q1_targeted_generation(
    const AdmittedPlacementRequest& admitted, PersistentWorkerTeam& team,
    TargetedGenerationControl& control, std::size_t target_index, TargetedWorkerOperation operation,
    WorkerCancellationPolicy cancellation)
    -> std::expected<TargetedWorkerGeneration, core::Error> {
    return PersistentWorkerTeamAccess::begin_targeted_generation_preallocated(
        admitted, team, control, target_index, operation, cancellation);
}

auto reset_monitoring_q1_targeted_control(TargetedGenerationControl& control) noexcept
    -> std::expected<void, core::Error> {
    return PersistentWorkerTeamAccess::reset_targeted_control(control);
}

} // namespace epycbench::runtime::detail
