#pragma once
#include "monitoring/host_q4_cpufreq_collector.hpp"

namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ4DurationSummary final { std::uint64_t count{},minimum{},lower_median{},maximum{}; };
struct MonitoringQ4EvidenceBinding final {
    std::string qualification_id,repository_commit,build_identity,profile_sha256,
        host_environment_digest,q0b_sha256,q0_commit,q1_sha256,q1_commit,q2_sha256,q2_commit,
        q3_sha256,q3_commit,manifest_sha256,checksums_sha256,plan_sha256;
    MonitoringQ4PlanValue plan;
    MonitoringQ4EvidenceBinding(std::string qualification,std::string repository,std::string build,
        std::string profile,std::string environment,std::string q0b,std::string q0c,
        std::string q1,std::string q1c,std::string q2,std::string q2c,std::string q3,
        std::string q3c,std::string manifest,std::string checksums,std::string plan_hash,
        MonitoringQ4PlanValue plan_value) noexcept
        : qualification_id{std::move(qualification)},repository_commit{std::move(repository)},
          build_identity{std::move(build)},profile_sha256{std::move(profile)},
          host_environment_digest{std::move(environment)},q0b_sha256{std::move(q0b)},q0_commit{std::move(q0c)},
          q1_sha256{std::move(q1)},q1_commit{std::move(q1c)},q2_sha256{std::move(q2)},q2_commit{std::move(q2c)},
          q3_sha256{std::move(q3)},q3_commit{std::move(q3c)},manifest_sha256{std::move(manifest)},
          checksums_sha256{std::move(checksums)},plan_sha256{std::move(plan_hash)},plan{std::move(plan_value)} {}
};
struct MonitoringQ4EvidenceDiscovery final {
    std::uint64_t cycle_ordinal{}; bool final{}; MonitoringQ4CpuFreqInterval interval;
    std::uint64_t policy_count{};
};
struct MonitoringQ4EvidenceDomain final {
    std::uint64_t ordinal{}; std::vector<core::LinuxCpuId> cpus;
    monitoring::FrequencySourceKind source_kind{}; std::string driver,governor;
    std::uint64_t minimum_hz{},maximum_hz{};
};
struct MonitoringQ4EvidencePreparation final {
    std::uint64_t cycle_ordinal{}; core::LinuxCpuId cpu{0};
    std::optional<std::uint64_t> domain_ordinal;
    std::optional<MonitoringQ4CpuFreqUnsupportedReason> unsupported;
    MonitoringQ4CpuFreqInterval interval;
};
struct MonitoringQ4EvidenceRead final {
    std::uint64_t cycle_ordinal{},round_ordinal{},domain_ordinal{},value_hz{};
    MonitoringQ4CpuFreqInterval interval;
};
class MonitoringQ4CpuFreqEvidenceValue final {
  public:
    static constexpr std::string_view schema="epycbench.monitoring-host-q4-frequency-evidence/v1";
    static constexpr std::string_view proof_limitation=
        "linux-cpufreq-reported-frequency-not-effective-execution-frequency/v1";
    explicit MonitoringQ4CpuFreqEvidenceValue(MonitoringQ4EvidenceBinding value) noexcept
        : binding{std::move(value)} {}
    MonitoringQ4EvidenceBinding binding;
    bool unsupported{};
    std::uint64_t admitted_count{},policy_count{},domain_count{};
    std::vector<MonitoringQ4EvidenceDiscovery> discoveries;
    std::vector<MonitoringQ4EvidenceDomain> domains;
    std::vector<MonitoringQ4EvidencePreparation> preparations;
    std::vector<MonitoringQ4EvidenceRead> agreements,typed_reads;
    MonitoringQ4DurationSummary inventory_durations,preparation_durations,raw_durations,typed_durations;
    bool production_sources_retired{};
    std::uint64_t capacity_bytes{};
};
class MonitoringQ4CpuFreqEvidenceBuilder final {
  public:
    static auto build(const MonitoringQ4SourceReady&,MonitoringQ4CpuFreqCollection&) noexcept
        -> std::expected<MonitoringQ4CpuFreqEvidenceValue,core::Error>;
};
}
