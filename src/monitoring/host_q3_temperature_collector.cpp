#include "monitoring/host_q3_temperature_collector.hpp"
namespace epycbench::monitoring::qualification::detail {namespace{
auto error(std::string c)->core::Error{return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q3.temperature.collection_failed"},.message="Q3 temperature collection failed",
 .operation="collect_monitoring_q3_temperature",.os_error=std::nullopt,.context={{"classification",std::move(c)}},
 .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};}
}
auto MonitoringQ3TemperatureCollector::collect(const MonitoringQ3PlanValue& plan,Operations o) noexcept
 ->std::expected<MonitoringQ3TemperatureCollection,MonitoringQ3TemperatureCollectionFailure>{
 MonitoringQ3TemperatureCollection out;bool active=false;std::optional<core::Error> primary;MonitoringQ3CollectionStage failed_stage=MonitoringQ3CollectionStage::finalization;
 const auto fail=[&](MonitoringQ3CollectionStage stage,core::Error value){if(!primary){failed_stage=stage;primary.emplace(std::move(value));}};
 const auto cleanup=[&](std::size_t cycle,MonitoringQ3TemperatureCollectionFailure& failure)noexcept{
  if(!active) return;
  active=false;
  auto retired=o.retire(o.context,cycle);
  if(!retired){
   if(failure.cleanup_count<failure.cleanup.size())failure.cleanup[failure.cleanup_count++].emplace(std::move(retired.error()));else failure.cleanup_overflowed=true;}};
 struct Diagnostics{core::Error deadline,clock,inter,identity,absence,retirement,contained;};
 std::optional<Diagnostics> diagnostic_stock;
 try{if(plan.discovery_cycles!=3||plan.agreement_rounds_per_cycle!=16||plan.raw_reads_per_agreement_round!=1||
  plan.typed_reads_per_agreement_round!=1||plan.execution_deadline_seconds!=60)return std::unexpected{MonitoringQ3TemperatureCollectionFailure{MonitoringQ3CollectionStage::capacity,error("plan"),{},0,false}};
  diagnostic_stock.emplace(Diagnostics{error("deadline"),error("clock_reversal"),error("inter_interval_reversal"),
   error("identity"),error("rediscovery_absent"),error("retirement"),error("contained")});
  auto& diagnostics=*diagnostic_stock;
  auto origin=o.now(o.context);if(!origin)return std::unexpected{MonitoringQ3TemperatureCollectionFailure{MonitoringQ3CollectionStage::clock,std::move(origin.error()),{},0,false}};
  auto last_end=*origin;
  const auto deadline=[&](std::uint64_t value){if(value<*origin){fail(MonitoringQ3CollectionStage::clock,std::move(diagnostics.clock));return false;}
   if(value-*origin>60000000000ULL){fail(MonitoringQ3CollectionStage::deadline,std::move(diagnostics.deadline));return false;}return true;};
  const auto checkpoint=[&]{auto value=o.now(o.context);if(!value){fail(MonitoringQ3CollectionStage::clock,std::move(value.error()));return false;}
   if(*value<last_end){fail(MonitoringQ3CollectionStage::clock,std::move(diagnostics.inter));return false;}last_end=*value;return deadline(*value);};
  const auto interval=[&](MonitoringQ3CollectionStage,auto operation,MonitoringQ3TemperatureInterval& recorded){auto before=o.now(o.context);if(!before){fail(MonitoringQ3CollectionStage::clock,std::move(before.error()));return false;}
   if(*before<last_end){fail(MonitoringQ3CollectionStage::clock,std::move(diagnostics.inter));return false;}if(!deadline(*before)||!operation())return false;
   auto after=o.now(o.context);if(!after){fail(MonitoringQ3CollectionStage::clock,std::move(after.error()));return false;}if(*after<*before){fail(MonitoringQ3CollectionStage::clock,std::move(diagnostics.clock));return false;}
   recorded={*before,*after};last_end=*after;if(!deadline(*after))return false;return true;};
  for(std::size_t cycle{};cycle<3;++cycle){bool supported=false;
   if(!interval(MonitoringQ3CollectionStage::source_discovery,[&]{auto v=o.discover(o.context,cycle);if(!v){fail(MonitoringQ3CollectionStage::source_discovery,std::move(v.error()));return false;}supported=*v;return true;},out.discovery[cycle]))goto rejected;
   ++out.discoveries;if(!supported){if(cycle==0){out.disposition=MonitoringQ3TemperatureDisposition::unsupported;return out;}fail(MonitoringQ3CollectionStage::source_discovery,std::move(diagnostics.absence));goto rejected;}active=true;
   for(std::size_t round{};round<16;++round){const auto index=cycle*16U+round;
    if(!interval(MonitoringQ3CollectionStage::agreement,[&]{auto v=o.raw(o.context,cycle,round);if(!v){fail(MonitoringQ3CollectionStage::agreement,std::move(v.error()));return false;}out.agreed_values[index]=*v;return true;},out.raw[index]))goto rejected;
    ++out.agreements;
    if(!interval(MonitoringQ3CollectionStage::source_read,[&]{auto v=o.typed(o.context,cycle,round);if(!v){fail(MonitoringQ3CollectionStage::source_read,std::move(v.error()));return false;}out.typed_values[index]=*v;return true;},out.typed[index]))goto rejected;
    ++out.typed_reads;}
   if(!checkpoint())goto rejected;
   {auto identity=o.identity(o.context,cycle);if(!identity){fail(MonitoringQ3CollectionStage::source_discovery,std::move(identity.error()));goto rejected;}if(!*identity){fail(MonitoringQ3CollectionStage::source_discovery,std::move(diagnostics.identity));goto rejected;}}
   if(!checkpoint())goto rejected;
   {active=false;auto retired=o.retire(o.context,cycle);if(!retired)
     return std::unexpected{MonitoringQ3TemperatureCollectionFailure{MonitoringQ3CollectionStage::finalization,std::move(retired.error()),{},0,false}};}
   if(!checkpoint())goto rejected;
   if(cycle>0)++out.rediscoveries;
  }
  if(!checkpoint())goto rejected;
  out.disposition=MonitoringQ3TemperatureDisposition::pass;return out;
 rejected: {MonitoringQ3TemperatureCollectionFailure f{failed_stage,primary?std::move(*primary):std::move(diagnostics.contained),{},0,false};
   cleanup(out.discoveries?out.discoveries-1U:0U,f);return std::unexpected{std::move(f)};}
 }catch(...){MonitoringQ3TemperatureCollectionFailure f{failed_stage,primary?std::move(*primary):
   diagnostic_stock?std::move(diagnostic_stock->contained):error("precollection_contained"),{},0,false};
  cleanup(out.discoveries?out.discoveries-1U:0U,f);return std::unexpected{std::move(f)};}}
}
