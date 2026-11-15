#pragma once
#include "monitoring/host_q3_authority_session.hpp"
#include "monitoring/host_q3_temperature_collector.hpp"
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ3TemperatureEvidenceTestAccess;
struct MonitoringQ3DurationSummary{std::uint64_t count{},minimum{},lower_median{},maximum{};};
enum class MonitoringQ3Support:unsigned char{supported,unsupported};
enum class MonitoringQ3SemanticCheck:unsigned char{passed,not_started};
enum class MonitoringQ3ParserResult:unsigned char{accepted};
enum class MonitoringQ3TypedReadStatus:unsigned char{available};
enum class MonitoringQ3ResourceFinalization:unsigned char{retired,not_started};
struct MonitoringQ3DiscoveryEvidence{std::uint64_t cycle{};MonitoringQ3TemperatureInterval interval{};
 MonitoringQ3Support support{};MonitoringQ3SemanticCheck semantic{};};
struct MonitoringQ3AgreementEvidence{std::uint64_t cycle{},round{};MonitoringQ3TemperatureInterval interval{};std::int64_t value{};
 MonitoringQ3ParserResult production_parser{MonitoringQ3ParserResult::accepted},oracle_parser{MonitoringQ3ParserResult::accepted};};
struct MonitoringQ3TypedReadEvidence{std::uint64_t cycle{},round{};MonitoringQ3TemperatureInterval interval{};std::int64_t value{};
 MonitoringQ3TypedReadStatus status{MonitoringQ3TypedReadStatus::available};};
struct MonitoringQ3EvidenceBinding{std::string qualification_id,repository_commit,build_identity,profile_sha256,
 host_environment_digest,q0b_sha256,q0_commit,q1_sha256,q1_commit,q2_sha256,q2_commit,manifest_sha256,checksums_sha256,plan_sha256;
 MonitoringQ3PlanValue plan;};
struct MonitoringQ3TemperatureEvidenceValue{static constexpr std::string_view schema="epycbench.monitoring-host-q3-temperature-evidence/v1";
 static constexpr std::string_view proof_limitation="same-input-bytes-not-independent-linux-k10temp-semantics/v1";
 MonitoringQ3EvidenceBinding binding;MonitoringQ3TemperatureDisposition disposition{};
 std::array<MonitoringQ3DiscoveryEvidence,3> discoveries{};std::size_t discovery_count{};
 std::array<MonitoringQ3AgreementEvidence,48> agreements{};std::array<MonitoringQ3TypedReadEvidence,48> typed{};
 std::size_t agreement_count{},typed_count{};
 MonitoringQ3DurationSummary discovery_durations{},raw_durations{},typed_durations{};
 MonitoringQ3ResourceFinalization authority{MonitoringQ3ResourceFinalization::retired},source{},oracle{};};
class MonitoringQ3TemperatureEvidenceBuilder{public:
 static auto build(const MonitoringQ3SourceReady&,const MonitoringQ3TemperatureCollection&)
  ->std::expected<MonitoringQ3TemperatureEvidenceValue,core::Error>;
private:
 friend class MonitoringQ3TemperatureEvidenceTestAccess;
 static auto binding(const MonitoringQ3SourceReady&)->MonitoringQ3EvidenceBinding;
 static auto build_bound(MonitoringQ3EvidenceBinding,const MonitoringQ3TemperatureCollection&)
  ->std::expected<MonitoringQ3TemperatureEvidenceValue,core::Error>;
};
}
