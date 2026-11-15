#include "monitoring/host_q2_historical_q1_git_authority.hpp"
#include "monitoring/host_q1_git_spawn.hpp"

namespace epycbench::monitoring::qualification::detail {
namespace {
auto rejected(std::string classification="historical_git") -> MonitoringQ2HistoricalFailure {
  return {{.category=core::ErrorCategory::monitoring,
    .code=core::StableErrorCode{"monitoring.q2.historical_q1_git.invalid"},
    .message="Historical Q1 Git authority rejected",.operation="authenticate_historical_q1_git",
    .os_error=std::nullopt,.context={{"classification",std::move(classification)}},
    .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command}};
}
struct DiagnosticStock final {
 MonitoringQ2HistoricalFailure generic=rejected(),current=rejected("current_capture_head"),ancestry=rejected("historical_ancestry"),q0_ancestry=rejected("q0_q1_ancestry"),tree=rejected("historical_tree"),
  tree_parse=rejected("historical_tree_parse"),blob=rejected("historical_blob"),bytes=rejected("historical_bytes"),
  final_capture=rejected("final_capture"),overflow=rejected("command_cleanup_overflow"),contained=rejected("contained");
};
auto hex_commit(std::string_view value) noexcept->bool{return (value.size()==40||value.size()==64)&&
  std::ranges::all_of(value,[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
auto transport(MonitoringQ1GitCommandFailure failure,MonitoringQ2HistoricalFailure overflow) -> MonitoringQ2HistoricalFailure {
  if(failure.cleanup.size()>16U){overflow.primary.os_error=failure.primary.os_error;return overflow;}
  MonitoringQ2HistoricalFailure result{std::move(failure.primary)};
  result.cleanup_count=failure.cleanup.size();
  for(std::size_t i{};i<result.cleanup_count;++i)result.cleanup[i].emplace(std::move(failure.cleanup[i]));
  return result;
}
}
auto MonitoringQ2HistoricalQ1GitAuthority::authenticate(
  const MonitoringQ1GitExecutable& executable,
  const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
  const MonitoringQ1GitRepositoryMetadata& metadata,std::string_view historical,
  std::string_view current,std::string_view q0,std::string_view profile,std::string_view q1_plan)
  noexcept ->std::expected<void,MonitoringQ2HistoricalFailure>{
  return authenticate_with(executable,root,metadata,historical,current,q0,profile,q1_plan,{});
}
auto MonitoringQ2HistoricalQ1GitAuthority::authenticate_with(
  const MonitoringQ1GitExecutable& executable,
  const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
  const MonitoringQ1GitRepositoryMetadata& metadata,std::string_view historical,
  std::string_view current,std::string_view q0,std::string_view profile,std::string_view q1_plan,TestOperations operations)
  noexcept->std::expected<void,MonitoringQ2HistoricalFailure>{
  std::optional<DiagnosticStock> diagnostics;
  try{diagnostics.emplace();}catch(...){core::Error emergency{};emergency.category=core::ErrorCategory::monitoring;emergency.continuation=core::ContinuationPolicy::abort_command;return std::unexpected{MonitoringQ2HistoricalFailure{std::move(emergency)}};}
  try{
    if(!hex_commit(historical)||!hex_commit(current)||!hex_commit(q0)||historical.size()!=current.size()||q0.size()!=historical.size())return std::unexpected{std::move(diagnostics->generic)};
    if(operations.fault)operations.fault(operations.context,0);
    auto current_capture=MonitoringQ1GitFixedResultAuthority::run_with_q2_plan(executable,root,metadata);
    if(!current_capture)return std::unexpected{transport(std::move(current_capture.error()),std::move(diagnostics->overflow))};
    if(operations.fault)operations.fault(operations.context,1);
    if(current_capture->head_!=current)return std::unexpected{std::move(diagnostics->current)};
    auto ancestry=MonitoringQ1GitSpawn::run_historical_ancestry(executable,root,historical,current);
    if(!ancestry)return std::unexpected{std::move(diagnostics->ancestry)};
    if(!ancestry->standard_output.empty()||!ancestry->standard_error.empty())return std::unexpected{std::move(diagnostics->ancestry)};
    auto q0_ancestry=MonitoringQ1GitSpawn::run_historical_ancestry(executable,root,q0,historical);
    if(!q0_ancestry)return std::unexpected{std::move(diagnostics->q0_ancestry)};
    if(!q0_ancestry->standard_output.empty()||!q0_ancestry->standard_error.empty())return std::unexpected{std::move(diagnostics->q0_ancestry)};
    constexpr std::array paths{"config/monitoring-qualification/host-v2-profile.json",
      "config/monitoring-qualification/host-v2-q1-page-fault-collection-plan.json"};
    constexpr std::array invocations{MonitoringQ1GitInvocation::historical_tree_profile,
      MonitoringQ1GitInvocation::historical_tree_q1_plan};
    const std::array expected{profile,q1_plan};
    int binding{};
    for(std::size_t i{};i<2;++i){
      auto tree=MonitoringQ1GitSpawn::run_historical_tree(executable,root,invocations[i],historical);
      if(!tree)return std::unexpected{transport(std::move(tree.error()),std::move(diagnostics->overflow))};
      if(!tree->standard_error.empty())return std::unexpected{std::move(diagnostics->tree)};
      auto object=MonitoringQ1GitFixedResultAuthority::parse_tree(tree->standard_output,paths[i],historical);
      if(!object)return std::unexpected{std::move(diagnostics->tree_parse)};
      MonitoringQ1GitObjectId token{std::move(*object),&binding,historical.size(),i};
      auto blob=MonitoringQ1GitSpawn::run_blob(executable,root,token,&binding,historical.size(),i);
      if(!blob)return std::unexpected{transport(std::move(blob.error()),std::move(diagnostics->overflow))};
      if(!blob->standard_error.empty())return std::unexpected{std::move(diagnostics->blob)};
      if(blob->standard_output!=expected[i])return std::unexpected{std::move(diagnostics->bytes)};
    }
    auto final_capture=MonitoringQ1GitFixedResultAuthority::run_with_q2_plan(executable,root,metadata);
    if(!final_capture)return std::unexpected{transport(std::move(final_capture.error()),std::move(diagnostics->overflow))};
    if(final_capture->head_!=current)return std::unexpected{std::move(diagnostics->final_capture)};
    return {};
  }catch(...){return std::unexpected{std::move(diagnostics->contained)};}
}
auto MonitoringQ2HistoricalQ1GitAuthority::transport_for_test(MonitoringQ1GitCommandFailure value) noexcept
 ->MonitoringQ2HistoricalFailure{try{auto overflow=rejected("command_cleanup_overflow");return transport(std::move(value),std::move(overflow));}catch(...){core::Error emergency{};emergency.category=core::ErrorCategory::monitoring;emergency.continuation=core::ContinuationPolicy::abort_command;return {std::move(emergency)};}}
auto MonitoringQ2HistoricalQ1GitAuthority::command_for_test(void* context,
 MonitoringQ1GitCommandFailure (*command)(void*)) noexcept->MonitoringQ2HistoricalFailure{
 try{auto overflow=rejected("command_cleanup_overflow");return transport(command(context),std::move(overflow));}catch(...){core::Error emergency{};emergency.category=core::ErrorCategory::monitoring;emergency.continuation=core::ContinuationPolicy::abort_command;return {std::move(emergency)};}}
} // namespace epycbench::monitoring::qualification::detail
