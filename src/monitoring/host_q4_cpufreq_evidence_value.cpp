#include "monitoring/host_q4_cpufreq_evidence_value.hpp"
#include <algorithm>
namespace epycbench::monitoring::qualification::detail { namespace {
thread_local bool builder_test_fault{};
auto error(std::string classification)->core::Error{const bool terminal=classification=="contained"||classification=="allocation";return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{terminal?"monitoring.q4.terminal_containment":"monitoring.q4.evidence_invalid"},.message="Q4 frequency evidence invalid",
 .operation="build_monitoring_q4_frequency_evidence",.os_error=std::nullopt,
 .context={{"classification",std::move(classification)}},.severity=core::ErrorSeverity::error,
 .continuation=core::ContinuationPolicy::abort_command};}
auto summary(std::span<const MonitoringQ4CpuFreqInterval> intervals)
 ->std::expected<MonitoringQ4DurationSummary,core::Error>{try{std::vector<std::uint64_t> values;values.reserve(intervals.size());
 for(const auto&i:intervals){if(i.end_ns<i.start_ns)return std::unexpected{error("interval")};values.push_back(i.end_ns-i.start_ns);}
 if(values.empty())return MonitoringQ4DurationSummary{};
 std::ranges::sort(values);return MonitoringQ4DurationSummary{
 values.size(),values.front(),values[(values.size()-1U)/2U],values.back()};}catch(...){return std::unexpected{error("allocation")};}}
template<class T>auto timed_intervals(std::span<const MonitoringQ4CpuFreqTimedFact<T>> facts){std::vector<MonitoringQ4CpuFreqInterval> out;out.reserve(facts.size());for(const auto&f:facts)out.push_back(f.interval);return out;}
}
void monitoring_q4_evidence_builder_test_fault(bool value) noexcept { builder_test_fault=value; }
auto MonitoringQ4CpuFreqEvidenceBuilder::build(const MonitoringQ4SourceReady&r,MonitoringQ4CpuFreqCollection&c) noexcept
 ->std::expected<MonitoringQ4CpuFreqEvidenceValue,core::Error>{try{
  if(builder_test_fault)throw std::bad_alloc{};
  if(!r.binding_||c.binding_!=r.binding_||c.projected_)return std::unexpected{error("source_ready_binding")};
  const bool unsupported=c.unsupported();const auto admitted=r.inputs().admitted_cpus();
  if(c.cycles().size()!=(unsupported?1U:3U)||c.discoveries().size()!=(unsupported?1U:6U)||
     c.preparations().size()!=(unsupported?admitted.size():3U*admitted.size())||
     (!unsupported&&c.raw_reads().size()!=c.typed_reads().size())||
     (unsupported&&(!c.raw_reads().empty()||!c.typed_reads().empty())))return std::unexpected{error("counts")};
  const auto& a=r.authority();const auto& provenance=a.evidence_provenance();const auto& history=a.history();
  MonitoringQ4CpuFreqEvidenceValue out{MonitoringQ4EvidenceBinding{std::string{a.qualification_id()},std::string{a.captured_head()},
   "sha256:"+a.build_identity().to_hex(),provenance.profile.to_hex(),"sha256:"+provenance.predecessor[0].to_hex(),
   provenance.predecessor[1].to_hex(),std::string{history.q0_commit()},provenance.predecessor[2].to_hex(),std::string{history.q1_commit()},
   provenance.predecessor[3].to_hex(),std::string{history.q2_commit()},provenance.predecessor[4].to_hex(),std::string{history.q3_commit()},
   provenance.predecessor[5].to_hex(),provenance.predecessor[6].to_hex(),a.governed().plan_sha256().to_hex(),a.governed().plan()}};
  out.unsupported=unsupported;out.admitted_count=admitted.size();out.policy_count=c.cycles().front().initial().policy_count();
  out.domain_count=c.cycles().front().initial().domains().size();
  out.discoveries.reserve(c.discoveries().size());
  for(std::size_t i{};i<c.discoveries().size();++i)out.discoveries.push_back({unsupported?1U:i/2U+1U,!unsupported&&i%2U==1U,c.discoveries()[i],out.policy_count});
  for(std::size_t i{};i<c.cycles().front().initial().domains().size();++i){const auto&d=c.cycles().front().initial().domains()[i];
   MonitoringQ4EvidenceDomain value{i+1U,{},d.source_kind,d.driver,d.governor,d.minimum_hz,d.maximum_hz};value.cpus.assign(d.cpus.begin(),d.cpus.end());out.domains.push_back(std::move(value));}
  out.preparations.reserve(c.preparations().size());for(const auto&p:c.preparations())out.preparations.push_back({p.cycle_ordinal,p.fact.cpu,p.fact.domain_ordinal,p.fact.unsupported,p.interval});
  out.agreements.reserve(c.raw_reads().size());for(const auto&v:c.raw_reads())out.agreements.push_back({v.cycle_ordinal,v.fact.round_ordinal,v.fact.domain_ordinal,v.fact.value_hz,v.interval});
  out.typed_reads.reserve(c.typed_reads().size());for(const auto&v:c.typed_reads())out.typed_reads.push_back({v.cycle_ordinal,v.fact.round_ordinal,v.fact.domain_ordinal,v.fact.value_hz,v.interval});
  auto prep=timed_intervals(c.preparations()),raw=timed_intervals(c.raw_reads()),typed=timed_intervals(c.typed_reads());
  auto is=summary(c.discoveries()),ps=summary(prep),rs=summary(raw),ts=summary(typed);if(!is)return std::unexpected{std::move(is.error())};if(!ps)return std::unexpected{std::move(ps.error())};if(!rs)return std::unexpected{std::move(rs.error())};if(!ts)return std::unexpected{std::move(ts.error())};
  out.inventory_durations=*is;out.preparation_durations=*ps;out.raw_durations=*rs;out.typed_durations=*ts;
  out.production_sources_retired=std::ranges::any_of(out.preparations,[](const auto&v){return v.domain_ordinal.has_value();});
  out.capacity_bytes=(unsupported?r.capacity().unsupported_evidence_bytes():r.capacity().pass_evidence_bytes()).value;c.projected_=true;return out;
 }catch(...){return std::unexpected{error("contained")};}}
}
