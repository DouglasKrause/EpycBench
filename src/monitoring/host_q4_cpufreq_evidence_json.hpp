#pragma once
#include "monitoring/host_q4_cpufreq_evidence_value.hpp"
namespace epycbench::monitoring::qualification::detail {
// Durable intrinsic verification carries no historical kernel-maximum or
// allocation receipt, and cannot be used as a SourceReady/EvidenceValue.
class MonitoringQ4RetainedEvidence final {
  public:
    MonitoringQ4RetainedEvidence(const MonitoringQ4RetainedEvidence&) = delete;
    MonitoringQ4RetainedEvidence(MonitoringQ4RetainedEvidence&&) noexcept = default;
    [[nodiscard]] auto bytes() const noexcept -> std::string_view { return bytes_; }
    [[nodiscard]] auto producer_commit() const noexcept -> std::string_view { return producer_; }
    [[nodiscard]] auto producer_build_identity() const noexcept -> std::string_view { return build_; }
  private:
    MonitoringQ4RetainedEvidence(std::string bytes, std::string producer, std::string build) noexcept
        : bytes_{std::move(bytes)}, producer_{std::move(producer)}, build_{std::move(build)} {}
    static auto verify(const MonitoringQ4CompleteQ3PredecessorValue&,
                       const MonitoringQ4HistoricalGitValue&,
                       const MonitoringQ4GovernedPlanSchemaBinding&,
                       const MonitoringQ4GovernedInputCandidate&, std::string_view)
        -> std::expected<MonitoringQ4RetainedEvidence, core::Error>;
    static void set_parsed_test_hook(void (*)() noexcept) noexcept;
    std::string bytes_, producer_, build_;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class HostFrozenSourceAdmissionTestAccess;
};
[[nodiscard]] auto validate_monitoring_q4_resolved_schema_instance(
    std::string_view,std::string_view) noexcept -> bool;
[[nodiscard]] auto validate_monitoring_q4_resolved_schema_instance_typed(
    std::string_view,std::string_view) noexcept -> std::expected<bool,core::Error>;
[[nodiscard]] auto encode_monitoring_q4_cpufreq_evidence_json(const MonitoringQ4CpuFreqEvidenceValue&) noexcept
 ->std::expected<std::string,core::Error>;
[[nodiscard]] auto validate_monitoring_q4_cpufreq_evidence_json(const MonitoringQ4SourceReady&,std::string_view) noexcept
 ->std::expected<MonitoringQ4CpuFreqEvidenceValue,core::Error>;
}
