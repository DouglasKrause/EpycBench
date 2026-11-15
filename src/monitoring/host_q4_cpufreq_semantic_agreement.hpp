#pragma once

#include "epycbench/monitoring/frequency.hpp"
#include "monitoring/host_q4_cpufreq_inventory_authority.hpp"
#include "platform_linux/cpu_frequency_access.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4CpuFreqUnsupportedReason : unsigned char {
    no_resolvable_policy,
    no_current_attribute
};
enum class MonitoringQ4CpuFreqSemanticFailureKind : unsigned char {
    inactive,
    binding,
    production,
    oracle,
    technical_token_invalid_utf8,
    overlap,
    conflict,
    agreement,
    capacity,
    contained,
    hardware_access
};
enum class MonitoringQ4CpuFreqProductionFailure : unsigned char {
    none,
    discovery,
    ambiguous_policy,
    invalid_value,
    contained
};
enum class MonitoringQ4CpuFreqSemanticField : unsigned char {
    none,
    policy_count,
    support,
    reason,
    membership,
    source_kind,
    driver,
    governor,
    minimum,
    maximum
};
struct MonitoringQ4CpuFreqSemanticFailure final {
    MonitoringQ4CpuFreqSemanticFailureKind kind{};
    MonitoringQ4CpuFreqSemanticField field{};
    std::uint64_t ordinal{};
    MonitoringQ4CpuFreqProductionFailure production{};
    MonitoringQ4CapacityReason reason{MonitoringQ4CapacityReason::none};
    int native_error{};
};
struct MonitoringQ4CpuFreqDomain final {
    std::span<const core::LinuxCpuId> cpus;
    monitoring::FrequencySourceKind source_kind{};
    std::string driver;
    std::string governor;
    std::uint64_t minimum_hz{};
    std::uint64_t maximum_hz{};
    std::uint32_t private_policy_locator{};
};
struct MonitoringQ4CpuFreqCpuOutcome final {
    core::LinuxCpuId cpu{0};
    std::optional<std::uint64_t> domain_ordinal;
    std::optional<MonitoringQ4CpuFreqUnsupportedReason> unsupported;
    auto operator==(const MonitoringQ4CpuFreqCpuOutcome&) const noexcept -> bool = default;
};
struct MonitoringQ4CpuFreqOraclePolicyScratch final {
    std::size_t membership_offset{};
    std::size_t membership_count{};
    monitoring::FrequencySourceKind source_kind{};
    std::string_view driver;
    std::string_view governor;
    std::uint64_t minimum_hz{};
    std::uint64_t maximum_hz{};
    std::uint32_t locator{};
    bool no_current{};
};
class MonitoringQ4CpuFreqSemanticSnapshot final {
  public:
    MonitoringQ4CpuFreqSemanticSnapshot(const MonitoringQ4CpuFreqSemanticSnapshot&) = delete;
    MonitoringQ4CpuFreqSemanticSnapshot(MonitoringQ4CpuFreqSemanticSnapshot&&) noexcept = default;
    [[nodiscard]] auto policy_count() const noexcept { return policy_count_; }
    [[nodiscard]] auto domains() const noexcept -> std::span<const MonitoringQ4CpuFreqDomain> {
        return domains_;
    }
    [[nodiscard]] auto cpus() const noexcept -> std::span<const MonitoringQ4CpuFreqCpuOutcome> {
        return cpus_;
    }

  private:
    MonitoringQ4CpuFreqSemanticSnapshot(std::uint64_t p,
                                        std::vector<core::LinuxCpuId> memberships,
                                        std::vector<MonitoringQ4CpuFreqDomain> domains,
                                        std::vector<MonitoringQ4CpuFreqCpuOutcome> cpus) noexcept
        : policy_count_{p}, memberships_{std::move(memberships)},
          domains_{std::move(domains)}, cpus_{std::move(cpus)} {}
    std::uint64_t policy_count_{};
    std::vector<core::LinuxCpuId> memberships_;
    std::vector<MonitoringQ4CpuFreqDomain> domains_;
    std::vector<MonitoringQ4CpuFreqCpuOutcome> cpus_;
    friend class MonitoringQ4CpuFreqSemanticAgreement;
    friend class MonitoringQ4CpuFreqSemanticAgreementTestAccess;
    friend class MonitoringQ4CpuFreqCycleAuthorityTestAccess;
};

class MonitoringQ4CpuFreqSemanticWorkspace final {
  public:
    MonitoringQ4CpuFreqSemanticWorkspace(const MonitoringQ4CpuFreqSemanticWorkspace&) = delete;
    MonitoringQ4CpuFreqSemanticWorkspace(MonitoringQ4CpuFreqSemanticWorkspace&&) noexcept;
    [[nodiscard]] static auto prepare(const MonitoringQ4SourceReady&) noexcept
        -> std::expected<MonitoringQ4CpuFreqSemanticWorkspace, MonitoringQ4CpuFreqSemanticFailure>;

  private:
    struct PrepareOperations final {
        void* context{};
        void (*fault)(void*, unsigned){};
    };
    [[nodiscard]] static auto prepare_with(std::shared_ptr<const void>, std::uint32_t,
                                           std::uint64_t, std::uint64_t,
                                           std::span<const core::LinuxCpuId>,
                                           PrepareOperations) noexcept
        -> std::expected<MonitoringQ4CpuFreqSemanticWorkspace,
                         MonitoringQ4CpuFreqSemanticFailure>;
    MonitoringQ4CpuFreqSemanticWorkspace() = default;
    std::shared_ptr<const void> binding_;
    std::uint32_t maximum_cpu_{};
    std::uint64_t maximum_count_{};
    std::vector<core::LinuxCpuId> admitted_;
    std::vector<platform_linux::detail::CpuFrequencyPolicyView> views_;
    platform_linux::detail::CpuFrequencyBoundedScratch production_scratch_;
    std::vector<core::LinuxCpuId> oracle_parse_scratch_;
    std::vector<core::LinuxCpuId> oracle_memberships_;
    std::vector<MonitoringQ4CpuFreqOraclePolicyScratch> oracle_policies_;
    std::vector<core::LinuxCpuId> production_memberships_;
    std::vector<MonitoringQ4CpuFreqDomain> production_domains_;
    std::vector<MonitoringQ4CpuFreqDomain> oracle_domains_;
    std::vector<MonitoringQ4CpuFreqCpuOutcome> production_cpus_;
    std::vector<MonitoringQ4CpuFreqCpuOutcome> oracle_cpus_;
    bool consumed_{};
    friend class MonitoringQ4CpuFreqSemanticAgreement;
    friend class MonitoringQ4CpuFreqSemanticAgreementTestAccess;
};

class MonitoringQ4CpuFreqSemanticAgreement final {
  public:
    [[nodiscard]] static auto evaluate(MonitoringQ4CpuFreqSemanticWorkspace&&,
                                       const MonitoringQ4CpuFreqInventory&) noexcept
        -> std::expected<MonitoringQ4CpuFreqSemanticSnapshot, MonitoringQ4CpuFreqSemanticFailure>;

  private:
    enum class Fault : unsigned char {
        none,
        reject_production,
        reject_oracle,
        disagree_source,
        throw_allocation,
        throw_unknown
    };
    using ViewObserver = void (*)(void*, std::size_t, std::size_t, const char*, std::size_t);
    [[nodiscard]] static auto evaluate_with(MonitoringQ4CpuFreqSemanticWorkspace&&,
                                            const MonitoringQ4CpuFreqInventory&, Fault, void*,
                                            ViewObserver) noexcept
        -> std::expected<MonitoringQ4CpuFreqSemanticSnapshot,
                         MonitoringQ4CpuFreqSemanticFailure>;
    friend class MonitoringQ4CpuFreqSemanticAgreementTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
