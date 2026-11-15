#pragma once
#include "monitoring/host_q3_authority_session.hpp"
#include "monitoring/host_q3_temperature_collector.hpp"
#include "monitoring/host_q3_temperature_cycle_authority.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3TemperatureProductionAdapter final{
 public:
  [[nodiscard]] static auto collect(const MonitoringQ3SourceReady&) noexcept
   ->std::expected<MonitoringQ3TemperatureCollection,MonitoringQ3TemperatureCollectionFailure>;
 private:
  struct Provider{void* context{};
   std::expected<std::optional<MonitoringQ3TemperatureCycleAuthority>,MonitoringQ3TemperatureCycleFailure>(*discover)(void*,std::size_t){};
   std::expected<std::uint64_t,core::Error>(*now)(void*) noexcept{};
   void(*fault)(void*,int){};
   std::expected<std::optional<std::int64_t>,core::Error>(*typed_override)(void*,std::size_t,std::size_t){};};
  [[nodiscard]] static auto collect_with(const MonitoringQ3PlanValue&,Provider) noexcept
   ->std::expected<MonitoringQ3TemperatureCollection,MonitoringQ3TemperatureCollectionFailure>;
  static auto discover_callback(void*,std::size_t)->std::expected<bool,core::Error>;
  friend class MonitoringQ3TemperatureProductionAdapterTestAccess;
};
class MonitoringQ3TemperatureProductionAdapterTestAccess final{public:
 using Provider=MonitoringQ3TemperatureProductionAdapter::Provider;
 static auto collect(const MonitoringQ3PlanValue& plan,Provider provider)noexcept{
  return MonitoringQ3TemperatureProductionAdapter::collect_with(plan,provider);}};
}
