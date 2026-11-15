#include "monitoring/host_q0b_backend_driver.hpp"

#include "monitoring/host_q0b_c0_coordinator.hpp"
#include "monitoring/host_qualification_composition.hpp"
#include "epycbench/monitoring/host_environment.hpp"
#include "epycbench/monitoring/host_input_template.hpp"
#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "epycbench/runtime/linux_pairwise_production_host_operations_provider.hpp"
#include "epycbench/runtime/pairwise_production_operations_adapter.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"
#include "runtime/controller_process_production_authority.hpp"

#include <chrono>
#include <exception>
#include <format>
#include <ranges>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto fail(core::Error error)->Q0bBackendFailure{return {std::move(error),{}};}
auto safe_code(std::string_view value) noexcept -> bool {
 return !value.empty()&&value.size()<=128U&&value.front()>='a'&&value.front()<='z'&&
  std::ranges::all_of(value,[](char item){return (item>='a'&&item<='z')||
   (item>='0'&&item<='9')||item=='.'||item=='_'||item=='-';});
}
auto stage_failure(std::string_view stage,std::optional<std::string> underlying=std::nullopt)
 ->core::Error {
 nlohmann::json context{{"classification",stage}};
 if(underlying&&safe_code(*underlying))context["underlying_code"]=*underlying;
 return {.category=core::ErrorCategory::monitoring,
  .code=core::StableErrorCode{"monitoring.q0b.backend.stage_failure"},
  .message="Monitoring Q0b backend stage failed",
  .operation="run_monitoring_host_q0b_backend",.os_error=std::nullopt,
  .context=std::move(context),.severity=core::ErrorSeverity::error,
  .continuation=core::ContinuationPolicy::abort_command};
}
auto stage_error(core::Error value,std::string_view stage)->core::Error {
 if(runtime::ipc::project_failure(value))return value;
 return stage_failure(stage,value.code.value());
}
auto fail_at(core::Error value,std::string_view stage)->Q0bBackendFailure {
 return fail(stage_error(std::move(value),stage));
}
auto failure_at(Q0bBackendFailure value,std::string_view stage)->Q0bBackendFailure {
 value.primary=stage_error(std::move(value.primary),stage);
 for(auto& item:value.secondary)item=stage_error(std::move(item),stage);
 return value;
}
auto contained(std::string classification)->core::Error{return {
 .category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.host_q0b.backend.driver_failed"},
 .message="Monitoring host Q0b backend orchestration failed",
 .operation="monitoring.host_q0b.backend.run",.os_error=std::nullopt,
 .context={{"classification",std::move(classification)}},.severity=core::ErrorSeverity::error,
 .continuation=core::ContinuationPolicy::abort_command};}
auto production_load(void*,const std::filesystem::path& path) noexcept
 ->std::expected<MonitoringHostInputTemplateAuthority,core::Error>{return load_monitoring_host_input_template(path);}
auto production_acquire(void*,MonitoringHostInputTemplateAuthority input) noexcept
 ->std::expected<RunnerContextAcquisitionSession,core::Error>{return acquire_runner_context_session(std::move(input));}
auto production_operations(void*) noexcept->std::expected<HostQ0bBackendOperations,core::Error>{
 try{
  auto owner=std::make_shared<runtime::LinuxPairwiseProductionHostOperationsProvider>(
    runtime::make_linux_pairwise_production_host_operations_provider());
  auto adapter = owner->adapter();
  auto operations=adapter.calibration_operations();if(!operations)return std::unexpected{operations.error()};
  return HostQ0bBackendOperations{owner,std::move(*operations)};
 }catch(const std::exception&){return std::unexpected{contained("operations_factory_exception")};}
 catch(...){return std::unexpected{contained("operations_factory_unknown_failure")};}
}
auto production_clock(void*) noexcept->std::expected<std::string,core::Error>{
 try{return std::format("{:%FT%TZ}",std::chrono::floor<std::chrono::seconds>(
   std::chrono::system_clock::now()));}
 catch(...){return std::unexpected{contained("clock_failure")};}
}
}

auto run_host_q0b_backend_driver(const std::filesystem::path& repository_root,
 std::string_view qualification_id,runtime::ControllerProcessProductionAuthority& authority) noexcept
 ->std::expected<MonitoringHostQ0bBackendResult,Q0bBackendFailure>{
 const HostQ0bBackendDependencies dependencies{nullptr,production_load,production_acquire,
   production_operations,production_clock,nullptr};
 return run_host_q0b_backend_driver_with_dependencies(repository_root,qualification_id,authority,dependencies);
}

auto apply_host_q0b_failure_stage_for_test(Q0bBackendFailure value,std::string_view stage)
 ->Q0bBackendFailure{return project_host_q0b_failure_stage(std::move(value),stage);}

auto production_host_q0b_backend_dependencies() noexcept -> HostQ0bBackendDependencies {
 return {nullptr,production_load,production_acquire,production_operations,production_clock,nullptr};
}
auto project_host_q0b_failure_stage(Q0bBackendFailure value,std::string_view stage)
 ->Q0bBackendFailure{return failure_at(std::move(value),stage);}

auto run_host_q0b_backend_driver_with_dependencies(const std::filesystem::path& repository_root,
 std::string_view qualification_id,runtime::ControllerProcessProductionAuthority& authority,
 const HostQ0bBackendDependencies& dependencies) noexcept
 ->std::expected<MonitoringHostQ0bBackendResult,Q0bBackendFailure>{
 std::string_view stage="input_load";
 try{
  if(!dependencies.load||!dependencies.acquire||!dependencies.make_operations||!dependencies.clock)
   return std::unexpected{fail(contained("missing_dependency"))};
  const auto inject=[&]()->std::optional<Q0bBackendFailure>{
   if(!dependencies.before_stage)return std::nullopt;
   auto result=dependencies.before_stage(dependencies.context,stage);
   if(result)return std::nullopt;
   return fail_at(std::move(result.error()),stage);
  };
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto input=dependencies.load(dependencies.context,repository_root);
  if(!input)return std::unexpected{fail_at(std::move(input.error()),stage)};
  stage="session_acquisition";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto session=dependencies.acquire(dependencies.context,std::move(*input));
  if(!session)return std::unexpected{fail_at(std::move(session.error()),stage)};
  stage="environment_derivation";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto environment=derive_host_environment(*session);
  if(!environment)return std::unexpected{fail_at(std::move(environment.error()),stage)};
  stage="plan_resolution";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto plan=resolve_monitoring_host_qualification_plan(*session,*environment);
  if(!plan)return std::unexpected{fail_at(std::move(plan.error()),stage)};
  stage="composition";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto context=session->runner_context();if(!context)return std::unexpected{fail_at(std::move(context.error()),stage)};
  runtime::PairwiseProductionCompositionContext composition{context->get(),authority.request_ids()};
  auto inputs=compose_host_qualification_calibration_inputs(*plan,*session,composition);
  if(!inputs)return std::unexpected{fail_at(std::move(inputs.error()),stage)};
  if(dependencies.after_composition){
   auto boundary=dependencies.after_composition(dependencies.context);
   if(!boundary)return std::unexpected{fail_at(std::move(boundary.error()),stage)};
  }
  if(dependencies.prepare_baseline){
   auto boundary=dependencies.prepare_baseline(dependencies.context,*session,*plan,*inputs);
   if(!boundary)return std::unexpected{fail_at(std::move(boundary.error()),stage)};
  }
  stage="bandwidth_facts";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto bandwidth=project_monitoring_host_admitted_production_facts(*session,*plan,inputs->bandwidth(),
    HostQ0bFamily::bandwidth,runtime::PairwiseVariant::first);
  if(!bandwidth)return std::unexpected{fail_at(std::move(bandwidth.error()),stage)};
  stage="latency_facts";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto latency=project_monitoring_host_admitted_production_facts(*session,*plan,inputs->latency(),
    HostQ0bFamily::latency,runtime::PairwiseVariant::first);
  if(!latency)return std::unexpected{fail_at(std::move(latency.error()),stage)};
  stage="base_projection";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto base=project_monitoring_host_q0b_base_environment(*environment,*plan,*bandwidth,*latency);
  if(!base)return std::unexpected{fail_at(std::move(base.error()),stage)};
  stage="operations_factory";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto operations=dependencies.make_operations(dependencies.context);
  if(!operations)return std::unexpected{fail_at(std::move(operations.error()),stage)};
  stage="clock";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto collected_at=dependencies.clock(dependencies.context);
  if(!collected_at)return std::unexpected{fail_at(std::move(collected_at.error()),stage)};
  stage="coordinator";
  if(auto injected=inject())return std::unexpected{std::move(*injected)};
  auto result=coordinate_host_q0b_c0_calibrations(qualification_id,*collected_at,*environment,*session,
    operations->operations,*inputs,*plan,*bandwidth,*latency,*base,dependencies.stage_admission);
  if(!result)return std::unexpected{failure_at(Q0bBackendFailure{std::move(result.error().primary),
    std::move(result.error().secondary)},stage)};
  return std::move(*result);
 }catch(const std::exception&){return std::unexpected{fail(stage_failure(stage))};}
 catch(...){return std::unexpected{fail(stage_failure(stage))};}
}
} // namespace epycbench::monitoring::qualification::detail
