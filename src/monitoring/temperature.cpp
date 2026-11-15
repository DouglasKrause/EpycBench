#include "epycbench/monitoring/temperature.hpp"

#include "temperature_classifier_kernel.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::monitoring {
namespace {
auto error(std::string_view code, std::string_view classification) -> core::Error {
 return {.category=core::ErrorCategory::monitoring,.code=core::StableErrorCode{std::string{code}},
 .message="Temperature classification input is invalid",.operation="classify_temperature",
 .os_error=std::nullopt,.context={{"classification",classification}},
 .severity=core::ErrorSeverity::error,.continuation=core::ContinuationPolicy::abort_experiment};
}
auto production_read(void* context) noexcept -> std::expected<std::int64_t,core::Error> {
 return static_cast<platform_linux::HwmonTemperatureSource*>(context)->read();
}
auto range_of(std::int64_t minimum, std::int64_t maximum) noexcept -> std::uint64_t {
 return static_cast<std::uint64_t>(maximum) - static_cast<std::uint64_t>(minimum);
}
} // namespace

TemperatureSource::TemperatureSource(void* context, ReadOperation read,
 std::unique_ptr<platform_linux::HwmonTemperatureSource> source) noexcept
 : context_{context},read_{read},source_{std::move(source)} {}
TemperatureSource::TemperatureSource(TemperatureSource&&) noexcept=default;
auto TemperatureSource::operator=(TemperatureSource&&) noexcept -> TemperatureSource& = default;
TemperatureSource::~TemperatureSource() noexcept=default;
auto TemperatureSource::prepare() -> std::expected<std::optional<TemperatureSource>,core::Error> {
 auto prepared=platform_linux::HwmonTemperatureSource::discover();
 if(!prepared) return std::unexpected{std::move(prepared.error())};
 if(!*prepared) return std::optional<TemperatureSource>{};
 auto owned=std::make_unique<platform_linux::HwmonTemperatureSource>(std::move(**prepared));
 auto* context=owned.get();
 return std::optional<TemperatureSource>{TemperatureSource{context,production_read,std::move(owned)}};
}
auto TemperatureSource::support_callback(void*) noexcept -> MonitoringSourceSupport { return MonitoringSourceSupport::supported; }
auto TemperatureSource::read_callback(void* context) noexcept
 -> std::expected<MonitoringPayloadResult<TemperatureSnapshot>,core::Error> {
 auto& source=*static_cast<TemperatureSource*>(context); auto value=source.read_(source.context_);
 if(!value) return std::unexpected{std::move(value.error())};
 return MonitoringPayloadResult<TemperatureSnapshot>::available(TemperatureSnapshot::create(
  TemperatureSensorKind::cpu_control,TemperatureMilliCelsius::from_millicelsius(*value)));
}
auto TemperatureSource::operations() noexcept -> std::expected<MonitoringSourceOperations<TemperatureSnapshot>,core::Error> {
 return MonitoringSourceOperations<TemperatureSnapshot>::create(this,support_callback,read_callback);
}
auto TemperatureSource::implementation_id() -> MonitoringImplementationId {
 return MonitoringImplementationId::create("linux-hwmon-k10temp-tctl");
}

TemperatureObservationEvidence::TemperatureObservationEvidence(MonitoringSignalState state,
 std::optional<TemperatureMilliCelsius> minimum,std::optional<TemperatureMilliCelsius> maximum,
 std::optional<TemperatureRangeMilliCelsius> range,std::size_t count,core::Duration interval,
 TemperatureCaptureCompleteness completeness) noexcept
 :state_{state},minimum_{minimum},maximum_{maximum},range_{range},count_{count},interval_{interval},completeness_{completeness} {}
auto TemperatureObservationEvidence::sensor_kind() const noexcept -> TemperatureSensorKind{return TemperatureSensorKind::cpu_control;}
auto TemperatureObservationEvidence::state() const noexcept -> MonitoringSignalState{return state_;}
auto TemperatureObservationEvidence::observed_minimum() const noexcept -> const std::optional<TemperatureMilliCelsius>&{return minimum_;}
auto TemperatureObservationEvidence::observed_maximum() const noexcept -> const std::optional<TemperatureMilliCelsius>&{return maximum_;}
auto TemperatureObservationEvidence::observed_range() const noexcept -> const std::optional<TemperatureRangeMilliCelsius>&{return range_;}
auto TemperatureObservationEvidence::sample_count() const noexcept -> std::size_t{return count_;}
auto TemperatureObservationEvidence::sampling_interval() const noexcept -> core::Duration{return interval_;}
auto TemperatureObservationEvidence::capture_completeness() const noexcept -> TemperatureCaptureCompleteness{return completeness_;}

auto classify_temperature(platform_linux::RawMonotonicTimestamp start,
 platform_linux::RawMonotonicTimestamp end,core::Duration interval,
 TemperatureCaptureCompleteness completeness,
 std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>> acquisitions)
 -> std::expected<TemperatureObservationEvidence,core::Error> {
 std::vector<const TimestampedMonitoringAcquisition<TemperatureSnapshot>*> scratch(
     acquisitions.size());
 auto classified=detail::classify_temperature_bounded(
     start,end,interval,completeness,acquisitions,scratch);
 if(!classified){
  using Failure=detail::TemperatureClassifierFailure;
  const auto [code,reason]=[&]()->std::pair<std::string_view,std::string_view>{
   switch(classified.error()){
   case Failure::ordering:return {"monitoring.temperature.invalid_input","ordering"};
   case Failure::interval:return {"monitoring.temperature.invalid_input","interval"};
   case Failure::boundary_overlap:return {"monitoring.temperature.invalid_input","boundary_overlap"};
   case Failure::metadata:return {"monitoring.temperature.association_mismatch","metadata"};
   case Failure::sensor:return {"monitoring.temperature.association_mismatch","sensor"};
   case Failure::driver:return {"monitoring.temperature.capture_incomplete","driver"};
   case Failure::sample_count:return {"monitoring.temperature.capture_incomplete","sample_count"};
   case Failure::leading_gap:return {"monitoring.temperature.capture_incomplete","leading_gap"};
   case Failure::internal_gap:return {"monitoring.temperature.capture_incomplete","internal_gap"};
   case Failure::trailing_gap:return {"monitoring.temperature.capture_incomplete","trailing_gap"};
   }
   std::terminate();
  }();
  return std::unexpected{error(code,reason)};
 }
 return TemperatureObservationEvidence{
     classified->state,classified->minimum,classified->maximum,
     classified->range,classified->count,interval,completeness};
}

auto detail::classify_temperature_bounded(
 platform_linux::RawMonotonicTimestamp start,
 platform_linux::RawMonotonicTimestamp end,core::Duration interval,
 TemperatureCaptureCompleteness completeness,
 std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>> acquisitions,
 std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>*>
     available_scratch) noexcept
 -> std::expected<TemperatureClassification,TemperatureClassifierFailure> {
 if(end<start||interval.value()==0U)
  return std::unexpected{TemperatureClassifierFailure::interval};
 if(completeness==TemperatureCaptureCompleteness::incomplete)
  return std::unexpected{TemperatureClassifierFailure::driver};
 if(available_scratch.size()<acquisitions.size())
  std::terminate();
 bool unavailable=false; std::size_t available_count{};
 auto previous=start; bool first=true;
 for(const auto& item:acquisitions){
  if(!first&&item.collection_start()<previous)
   return std::unexpected{TemperatureClassifierFailure::ordering};
  first=false; previous=item.collection_start();
  if(item.channel()!=MonitoringChannel::temperature||
     item.phase()!=MonitoringSamplePhase::during_observation||
     item.implementation_id().value()!="linux-hwmon-k10temp-tctl")
   return std::unexpected{TemperatureClassifierFailure::metadata};
  if(item.collection_start()<start||item.collection_end()>end)
   return std::unexpected{TemperatureClassifierFailure::boundary_overlap};
  if(item.status()==MonitoringAcquisitionStatus::unavailable){unavailable=true;continue;}
  if(item.payload()->sensor_kind()!=TemperatureSensorKind::cpu_control)
   return std::unexpected{TemperatureClassifierFailure::sensor};
  available_scratch[available_count++]=&item;
 }
 const auto available=available_scratch.first(available_count);
 if(available.size()<2U&&!unavailable)
  return std::unexpected{TemperatureClassifierFailure::sample_count};
 if(!unavailable&&available.size()>=2U){
  const auto limit=interval.value();
  if(available.front()->collection_end().nanoseconds()-start.nanoseconds()>limit)
   return std::unexpected{TemperatureClassifierFailure::leading_gap};
  for(std::size_t i=1;i<available.size();++i)
   if(available[i]->collection_end().nanoseconds()-
          available[i-1]->collection_start().nanoseconds()>limit)
    return std::unexpected{TemperatureClassifierFailure::internal_gap};
  if(end.nanoseconds()-available.back()->collection_start().nanoseconds()>limit)
   return std::unexpected{TemperatureClassifierFailure::trailing_gap};
 }
 TemperatureClassification result;
 result.count=available.size();
 if(!available.empty()){
  auto minimum=available.front()->payload()->temperature().value();
  auto maximum=minimum;
  for(const auto* item:available){
   const auto value=item->payload()->temperature().value();
   minimum=std::min(minimum,value); maximum=std::max(maximum,value);
  }
  result.minimum=TemperatureMilliCelsius::from_millicelsius(minimum);
  result.maximum=TemperatureMilliCelsius::from_millicelsius(maximum);
  if(available.size()>=2U)
   result.range=TemperatureRangeMilliCelsius::from_millicelsius(
       range_of(minimum,maximum));
 }
 result.state=result.range&&result.range->value()>=5000U
     ? MonitoringSignalState::detected
     : unavailable?MonitoringSignalState::unavailable
                  : MonitoringSignalState::clear;
 return result;
}
} // namespace epycbench::monitoring
