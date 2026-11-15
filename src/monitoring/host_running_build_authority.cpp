#include "monitoring/host_running_build_authority.hpp"
#include "monitoring/host_running_build_attestation_value.hpp"
#include "epycbench/build_manifest.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <utility>
namespace epycbench::monitoring::qualification::detail { namespace {
auto diagnostic(std::string classification)->core::Error{return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q2.running_build.invalid"},.message="Running build authority rejected",
 .operation="authenticate_monitoring_running_build",.os_error=std::nullopt,
 .context={{"classification",std::move(classification)}},.severity=core::ErrorSeverity::error,
 .continuation=core::ContinuationPolicy::abort_command};}
auto production_manifest(void*)->nlohmann::json{return build_manifest_json();}void production_fault(void*,unsigned int){}
auto emergency() noexcept->core::Error{core::Error value{};value.category=core::ErrorCategory::monitoring;
 value.continuation=core::ContinuationPolicy::abort_command;return value;}
}
MonitoringRunningBuildAuthority::MonitoringRunningBuildAuthority(std::string commit,std::string canonical,
 core::Sha256Digest identity,Operations operations,core::Error recheck,core::Error retire,core::Error contained) noexcept
 :commit_{std::move(commit)},canonical_{std::move(canonical)},identity_{identity},operations_{operations},
 recheck_error_{std::move(recheck)},retire_error_{std::move(retire)},contained_error_{std::move(contained)}{}
MonitoringRunningBuildAuthority::MonitoringRunningBuildAuthority(MonitoringRunningBuildAuthority&& other) noexcept
 :commit_{std::move(other.commit_)},canonical_{std::move(other.canonical_)},identity_{other.identity_},
 operations_{other.operations_},recheck_error_{std::move(other.recheck_error_)},retire_error_{std::move(other.retire_error_)},
 contained_error_{std::move(other.contained_error_)},active_{std::exchange(other.active_,false)},checked_{other.checked_}{}
MonitoringRunningBuildAuthority::~MonitoringRunningBuildAuthority() noexcept{if(active_)static_cast<void>(retire());}
auto MonitoringRunningBuildAuthority::retain(std::string_view commit) noexcept
 ->std::expected<MonitoringRunningBuildAuthority,core::Error>{return retain_with(commit,{nullptr,production_manifest,production_fault});}
auto MonitoringRunningBuildAuthority::retain_with(std::string_view commit,Operations operations) noexcept
 ->std::expected<MonitoringRunningBuildAuthority,core::Error>{
 try{auto invalid=diagnostic("manifest");auto recheck=diagnostic("recheck");auto retire=diagnostic("retire");auto contained=diagnostic("contained");
  if(!operations.manifest||!operations.fault)return std::unexpected{std::move(invalid)};
  operations.fault(operations.context,0);auto build=operations.manifest(operations.context);
  auto attested=attest_monitoring_running_build_value(build,commit);if(!attested)return std::unexpected{std::move(attested.error())};
  if(!attested->at("build_identity").is_string())return std::unexpected{std::move(invalid)};
  auto identity=core::Sha256Digest::parse(attested->at("build_identity").get<std::string>());
  if(!identity)return std::unexpected{std::move(invalid)};
  auto canonical=core::canonicalize_json(*attested);if(!canonical)return std::unexpected{std::move(invalid)};
  return MonitoringRunningBuildAuthority{std::string{commit},std::move(*canonical),*identity,operations,
    std::move(recheck),std::move(retire),std::move(contained)};
 }catch(...){return std::unexpected{emergency()};}}
auto MonitoringRunningBuildAuthority::recheck() noexcept->std::expected<bool,core::Error>{
 if(!active_||checked_) {
  std::terminate();
 }
 checked_=true;
 try{if(operations_.recheck_failure){auto injected=operations_.recheck_failure(operations_.context);if(injected)return std::unexpected{std::move(*injected)};}
  operations_.fault(operations_.context,1);
  auto value=operations_.manifest(operations_.context);auto attested=attest_monitoring_running_build_value(value,commit_);
  if(!attested||attested->at("build_identity")!=identity_.to_hex()) {
   return false;
  }
  auto canonical=core::canonicalize_json(*attested);
  return canonical&&*canonical==canonical_;}catch(...){return std::unexpected{std::move(contained_error_)};}}
auto MonitoringRunningBuildAuthority::retire() noexcept->std::expected<void,core::Error>{
 if(!active_) {
  std::terminate();
 }
 active_=false;
 try{operations_.fault(operations_.context,2);return {};}
 catch(...){return std::unexpected{std::move(retire_error_)};}}
}
