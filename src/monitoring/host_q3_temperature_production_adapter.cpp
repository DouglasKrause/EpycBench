#include "monitoring/host_q3_temperature_production_adapter.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
namespace epycbench::monitoring::qualification::detail {namespace{
struct Context{MonitoringQ3TemperatureProductionAdapterTestAccess::Provider provider;
 std::optional<MonitoringQ3TemperatureCycleAuthority> cycle;
 std::optional<::epycbench::monitoring::TemperatureSource> typed;
 std::optional<::epycbench::monitoring::MonitoringSourceOperations<::epycbench::monitoring::TemperatureSnapshot>> operations;
 std::array<std::optional<core::Error>,32> cleanup{};std::size_t count{};bool overflow{};
 core::Error contained,unavailable;std::optional<MonitoringQ3ParserAgreementStage> raw_stage;};
auto error(std::string classification)->core::Error{return {.category=core::ErrorCategory::monitoring,
 .code=core::StableErrorCode{"monitoring.q3.temperature.adapter_failed"},.message="Q3 temperature adapter failed",
 .operation="collect_monitoring_q3_temperature",.os_error=std::nullopt,.context={{"classification",std::move(classification)}},
 .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_command};}
void retain(Context& c,MonitoringQ3TemperatureCycleFailure failure){for(std::size_t i{};i<failure.cleanup_count;++i){
 if(c.count<c.cleanup.size())c.cleanup[c.count++].emplace(std::move(*failure.cleanup[i]));else c.overflow=true;}}
void retain_one(Context& c,core::Error value){if(c.count<c.cleanup.size())c.cleanup[c.count++].emplace(std::move(value));else c.overflow=true;}
auto cycle_error(Context& c,MonitoringQ3TemperatureCycleFailure failure)->core::Error{auto primary=std::move(failure.primary);retain(c,std::move(failure));return primary;}
auto abort_discovery(Context& c,core::Error primary)->std::expected<bool,core::Error>{c.operations.reset();c.typed.reset();
 if(c.cycle){auto retired=c.cycle->retire();c.cycle.reset();if(!retired){auto retirement=std::move(retired.error());
  retain_one(c,std::move(retirement.primary));retain(c,std::move(retirement));}}
 return std::unexpected{std::move(primary)};}
}
auto MonitoringQ3TemperatureProductionAdapter::discover_callback(void* raw,std::size_t ordinal)->std::expected<bool,core::Error>{auto&c=*static_cast<Context*>(raw);
 auto value=c.provider.discover(c.provider.context,ordinal);if(!value)return std::unexpected{cycle_error(c,std::move(value.error()))};
 if(!*value)return false;
 try{c.cycle.emplace(std::move(**value));
  if(c.provider.fault)c.provider.fault(c.provider.context,1);
  c.typed.emplace(c.cycle->typed_source_for_adapter());
  if(c.provider.fault)c.provider.fault(c.provider.context,2);
  auto operations=c.typed->operations();
  if(!operations)return abort_discovery(c,std::move(operations.error()));
  c.operations.emplace(std::move(*operations));
  if(c.provider.fault)c.provider.fault(c.provider.context,3);
  return true;
 }catch(...){return abort_discovery(c,std::move(c.contained));}}
namespace {
auto raw(void* context,std::size_t,std::size_t)->std::expected<std::int64_t,core::Error>{auto&c=*static_cast<Context*>(context);
 auto value=c.cycle->acquire_raw();if(!value){c.raw_stage=value.error().raw_stage;return std::unexpected{cycle_error(c,std::move(value.error()))};}return value->value();}
auto typed(void* context,std::size_t cycle,std::size_t round)->std::expected<std::int64_t,core::Error>{auto&c=*static_cast<Context*>(context);
 if(c.provider.typed_override){auto overridden=c.provider.typed_override(c.provider.context,cycle,round);
  if(!overridden)return std::unexpected{std::move(overridden.error())};
  if(!*overridden)return std::unexpected{std::move(c.unavailable)};
  return **overridden;}
 auto value=c.operations->read();if(!value)return std::unexpected{std::move(value.error())};if(!value->payload())
  return std::unexpected{std::move(c.unavailable)};
 return value->payload()->temperature().value();}
auto identity(void* context,std::size_t)->std::expected<bool,core::Error>{auto&c=*static_cast<Context*>(context);auto value=c.cycle->recheck();
 if(!value)return std::unexpected{cycle_error(c,std::move(value.error()))};
 return *value;}
auto retire(void* context,std::size_t)noexcept->std::expected<void,core::Error>{auto&c=*static_cast<Context*>(context);c.operations.reset();c.typed.reset();
 auto value=c.cycle->retire();c.cycle.reset();if(!value)return std::unexpected{cycle_error(c,std::move(value.error()))};return {};}
auto production_discover(void*,std::size_t)->std::expected<std::optional<MonitoringQ3TemperatureCycleAuthority>,MonitoringQ3TemperatureCycleFailure>{return MonitoringQ3TemperatureCycleAuthority::discover();}
auto production_now(void*)noexcept->std::expected<std::uint64_t,core::Error>{auto value=platform_linux::raw_monotonic_now();if(!value)return std::unexpected{std::move(value.error())};return value->nanoseconds();}
}
auto MonitoringQ3TemperatureProductionAdapter::collect(const MonitoringQ3SourceReady& ready)noexcept
 ->std::expected<MonitoringQ3TemperatureCollection,MonitoringQ3TemperatureCollectionFailure>{return collect_with(ready.plan(),{nullptr,production_discover,production_now,nullptr,nullptr});}
auto MonitoringQ3TemperatureProductionAdapter::collect_with(const MonitoringQ3PlanValue& plan,Provider provider)noexcept
 ->std::expected<MonitoringQ3TemperatureCollection,MonitoringQ3TemperatureCollectionFailure>{
 MonitoringQ3TemperatureCollectionFailure bootstrap{};bootstrap.primary.category=core::ErrorCategory::monitoring;
 bootstrap.primary.severity=core::ErrorSeverity::error;bootstrap.primary.continuation=core::ContinuationPolicy::abort_command;
 std::optional<core::Error> initialization_failure;
 try{initialization_failure.emplace(error("initialization_contained"));
  if(provider.fault)provider.fault(provider.context,0);
  Context context{provider,{},{},{},{},0U,false,error("contained"),error("typed_unavailable"),{}};
 auto result=MonitoringQ3TemperatureCollector::collect(plan,{&context,provider.now,discover_callback,raw,typed,identity,retire});
 if(!result){
  if(context.raw_stage){
   result.error().stage=*context.raw_stage==MonitoringQ3ParserAgreementStage::oracle
      ?MonitoringQ3CollectionStage::oracle:MonitoringQ3CollectionStage::agreement;
  }
  for(std::size_t i{};i<context.count;++i){
   if(result.error().cleanup_count<result.error().cleanup.size()){
    result.error().cleanup[result.error().cleanup_count++].emplace(std::move(*context.cleanup[i]));
   }else{
    result.error().cleanup_overflowed=true;
   }
  }
  result.error().cleanup_overflowed=result.error().cleanup_overflowed||context.overflow;
 }
 return result;
 }catch(...){
  if(initialization_failure){
   return std::unexpected{MonitoringQ3TemperatureCollectionFailure{
    MonitoringQ3CollectionStage::finalization,std::move(*initialization_failure),{},0U,false}};
  }
  return std::unexpected{std::move(bootstrap)};
 }
}
}
