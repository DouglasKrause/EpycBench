#pragma once
#include "monitoring/host_q3_temperature_evidence_value.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3TemperatureEvidenceTestAccess final {
public:
 static auto build(MonitoringQ3EvidenceBinding binding,const MonitoringQ3TemperatureCollection& collection)
  ->std::expected<MonitoringQ3TemperatureEvidenceValue,core::Error>{
  return MonitoringQ3TemperatureEvidenceBuilder::build_bound(std::move(binding),collection);
 }
};
}
