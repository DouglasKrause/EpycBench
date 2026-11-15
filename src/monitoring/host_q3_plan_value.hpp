#pragma once
#include "epycbench/core/error.hpp"
#include <cstdint>
#include <expected>
#include <string_view>
#include <variant>
#include "monitoring/governed_plan_decode.hpp"
namespace epycbench::monitoring::qualification::detail {
using MonitoringQ3AuthoredDiagnostic=MonitoringGovernedPlanAuthoredDiagnostic;
enum class MonitoringQ3ProductionImplementation : unsigned char { linux_hwmon_k10temp_tctl };
enum class MonitoringQ3SensorKind : unsigned char { cpu_control };
enum class MonitoringQ3TemperatureUnit : unsigned char { millidegree_celsius };
enum class MonitoringQ3OracleProtocol : unsigned char { identity_bound_same_byte_dual_parser_v1 };
enum class MonitoringQ3AgreementRule : unsigned char { exact_int64_equality };
enum class MonitoringQ3PrivacyValidation : unsigned char { passed };
struct MonitoringQ3PlanValue final {
 static constexpr std::string_view schema="epycbench.monitoring-q3-temperature-collection-plan/v1";
 static constexpr std::string_view path="config/monitoring-qualification/host-v2-q3-temperature-collection-plan.json";
 std::uint64_t discovery_cycles{},agreement_rounds_per_cycle{},raw_reads_per_agreement_round{},typed_reads_per_agreement_round{},maximum_raw_input_bytes{},execution_deadline_seconds{};
 std::uint64_t backend_stdout_bytes{},backend_stderr_bytes{},canonical_evidence_bytes{},diagnostic_count{};
 MonitoringQ3ProductionImplementation production_implementation{MonitoringQ3ProductionImplementation::linux_hwmon_k10temp_tctl};
 MonitoringQ3SensorKind sensor_kind{MonitoringQ3SensorKind::cpu_control};
 MonitoringQ3TemperatureUnit unit{MonitoringQ3TemperatureUnit::millidegree_celsius};
 MonitoringQ3OracleProtocol oracle_protocol{MonitoringQ3OracleProtocol::identity_bound_same_byte_dual_parser_v1};
 MonitoringQ3AgreementRule agreement_rule{MonitoringQ3AgreementRule::exact_int64_equality};
 MonitoringQ3PrivacyValidation privacy_validation{MonitoringQ3PrivacyValidation::passed};
};
[[nodiscard]] auto decode_monitoring_q3_plan(std::string_view)->std::expected<MonitoringQ3PlanValue,core::Error>;
[[nodiscard]] auto decode_monitoring_q3_plan_authored(std::string_view)
 ->std::expected<MonitoringQ3PlanValue,MonitoringGovernedPlanDecodeFailure>;
}
