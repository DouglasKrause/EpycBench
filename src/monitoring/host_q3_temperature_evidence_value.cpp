#include "monitoring/host_q3_temperature_evidence_value.hpp"
#include <algorithm>
namespace epycbench::monitoring::qualification::detail{namespace{
auto error(std::string c)->core::Error{return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q3.evidence_invalid"},.message="Q3 evidence value invalid",
 .operation="build_monitoring_q3_temperature_evidence",.os_error=std::nullopt,.context={{"classification",std::move(c)}},
 .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};}
template<std::size_t N>auto summary(const std::array<MonitoringQ3TemperatureInterval,N>& values,std::size_t count)
 ->std::expected<MonitoringQ3DurationSummary,core::Error>{if(count>N)return std::unexpected{error("count")};
 std::array<std::uint64_t,N> durations{};for(std::size_t i{};i<count;++i){if(values[i].end<values[i].start)return std::unexpected{error("interval")};durations[i]=values[i].end-values[i].start;}
 if(count==0)return MonitoringQ3DurationSummary{};
 std::sort(durations.begin(),durations.begin()+static_cast<std::ptrdiff_t>(count));
 return MonitoringQ3DurationSummary{count,durations[0],durations[(count-1U)/2U],durations[count-1U]};}
}
auto MonitoringQ3TemperatureEvidenceBuilder::binding(const MonitoringQ3SourceReady& ready)->MonitoringQ3EvidenceBinding{
 const auto&p=ready.predecessor();return {std::string{ready.qualification_id()},std::string{ready.repository_commit()},
  "sha256:"+ready.build_identity().to_hex(),std::string{ready.profile_sha256()},p.host_environment_digest,
  p.q0b_evidence.to_hex(),p.q0_commit,p.q1_evidence.to_hex(),p.q1_commit,p.q2_evidence.to_hex(),p.q2_commit,
  p.manifest.to_hex(),p.checksums.to_hex(),ready.plan_sha256().to_hex(),ready.plan()};}
auto MonitoringQ3TemperatureEvidenceBuilder::build(const MonitoringQ3SourceReady& ready,const MonitoringQ3TemperatureCollection& c)
 ->std::expected<MonitoringQ3TemperatureEvidenceValue,core::Error>{return build_bound(binding(ready),c);}
auto MonitoringQ3TemperatureEvidenceBuilder::build_bound(MonitoringQ3EvidenceBinding binding,const MonitoringQ3TemperatureCollection& c)
 ->std::expected<MonitoringQ3TemperatureEvidenceValue,core::Error>{const bool pass=c.disposition==MonitoringQ3TemperatureDisposition::pass;
  if((pass&&(c.discoveries!=3||c.rediscoveries!=2||c.agreements!=48||c.typed_reads!=48))||
     (!pass&&(c.discoveries!=1||c.rediscoveries!=0||c.agreements!=0||c.typed_reads!=0)))return std::unexpected{error("counts")};
  auto ds=summary(c.discovery,c.discoveries),rs=summary(c.raw,c.agreements),ts=summary(c.typed,c.typed_reads);
  if(!ds)return std::unexpected{std::move(ds.error())};
  if(!rs)return std::unexpected{std::move(rs.error())};
  if(!ts)return std::unexpected{std::move(ts.error())};
  MonitoringQ3TemperatureEvidenceValue out;out.binding=std::move(binding);out.disposition=c.disposition;out.discovery_count=c.discoveries;
  std::uint64_t preceding{};for(std::size_t cycle{};cycle<c.discoveries;++cycle){if(c.discovery[cycle].start<preceding)return std::unexpected{error("ordering")};preceding=c.discovery[cycle].end;
   out.discoveries[cycle]={cycle+1U,c.discovery[cycle],pass?MonitoringQ3Support::supported:MonitoringQ3Support::unsupported,
    pass?MonitoringQ3SemanticCheck::passed:MonitoringQ3SemanticCheck::not_started};if(!pass)break;
   for(std::size_t round{};round<16;++round){const auto i=cycle*16U+round;if(c.raw[i].start<preceding||c.typed[i].start<c.raw[i].end)return std::unexpected{error("ordering")};
    preceding=c.typed[i].end;out.agreements[i]={cycle+1U,round+1U,c.raw[i],c.agreed_values[i]};out.typed[i]={cycle+1U,round+1U,c.typed[i],c.typed_values[i]};}}
  out.agreement_count=c.agreements;out.typed_count=c.typed_reads;out.discovery_durations=*ds;out.raw_durations=*rs;out.typed_durations=*ts;
  out.source=pass?MonitoringQ3ResourceFinalization::retired:MonitoringQ3ResourceFinalization::not_started;out.oracle=out.source;return out;}
}
