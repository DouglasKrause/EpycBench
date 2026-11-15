#pragma once
#include "monitoring/host_q3_temperature_evidence_value.hpp"
namespace epycbench::monitoring::qualification::detail {
[[nodiscard]] auto encode_monitoring_q3_temperature_evidence_json(const MonitoringQ3TemperatureEvidenceValue&)
 ->std::expected<std::string,core::Error>;
[[nodiscard]] auto validate_monitoring_q3_temperature_evidence_json(std::string_view)
 ->std::expected<MonitoringQ3TemperatureEvidenceValue,core::Error>;
}
