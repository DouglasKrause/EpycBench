#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"
#include "runtime/pairwise_monitoring_host_capacity_admission.hpp"

namespace epycbench::runtime::detail {
class PairwiseMonitoringOwningFacade;
struct MonitoringPreparationFactTestAccess;

enum class MonitoringPreparationFactDomain : unsigned char { source, synthetic };
enum class MonitoringPreparationSupport : unsigned char { not_requested, supported, unsupported };
struct MonitoringPreparationChannelFact final {
    MonitoringPreparationSupport support;
    monitoring::MonitoringEffectiveChannelDisposition effective;
    auto operator==(const MonitoringPreparationChannelFact&) const noexcept -> bool = default;
};
struct MonitoringPreparationServiceFact final {
    core::LinuxCpuId assigned, verified_singleton, observed;
    auto operator==(const MonitoringPreparationServiceFact&) const noexcept -> bool = default;
};
struct MonitoringPreparationFrequencyView final {
    std::span<const core::LinuxCpuId> cpus;
    monitoring::FrequencySourceKind kind;
    std::string_view driver, governor;
    std::uint64_t minimum_hz, maximum_hz;
};
// Fixed storage, published only after the complete two-stage acquisition succeeds.
// Source buffers remain owned by the facade, including after retirement.
class MonitoringPreparationCheckpoint final {
  public:
    auto service_placed(core::LinuxCpuId assigned, core::LinuxCpuId singleton,
                        core::LinuxCpuId observed) noexcept -> bool;
    auto complete(const monitoring::MonitoringExecutionPolicy&, std::size_t workers,
                  std::size_t page_sources, std::size_t context_sources,
                  bool temperature_supported, bool frequency_supported,
                  std::size_t frequency_domains) noexcept -> bool;
    [[nodiscard]] auto policy() const noexcept -> const std::optional<monitoring::MonitoringExecutionPolicy>& { return policy_; }
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t { return workers_; }
    [[nodiscard]] auto domain_count() const noexcept -> std::size_t { return domains_; }
    [[nodiscard]] auto completed() const noexcept -> bool { return completed_; }
    [[nodiscard]] auto channels() const noexcept -> const std::array<MonitoringPreparationChannelFact,4>& { return channels_; }
    [[nodiscard]] auto service() const noexcept -> const std::optional<MonitoringPreparationServiceFact>& { return service_; }
  private:
    bool completed_{};
    std::optional<monitoring::MonitoringExecutionPolicy> policy_;
    std::size_t workers_{}, domains_{};
    std::array<MonitoringPreparationChannelFact,4> channels_{};
    std::optional<MonitoringPreparationServiceFact> service_;
};

class MonitoringPreparationFactView final {
  public:
    [[nodiscard]] auto domain() const noexcept -> MonitoringPreparationFactDomain { return domain_; }
    [[nodiscard]] auto admission() const noexcept -> const PairwiseMonitoringHostCapacityAdmission& { return *admission_; }
    [[nodiscard]] auto workers() const noexcept -> std::span<const monitoring::MonitoringWorkerScope> { return workers_; }
    [[nodiscard]] auto channels() const noexcept -> const std::array<MonitoringPreparationChannelFact,4>& { return checkpoint_->channels(); }
    [[nodiscard]] auto service() const noexcept -> const std::optional<MonitoringPreparationServiceFact>& { return checkpoint_->service(); }
    [[nodiscard]] auto frequency_domains() const noexcept -> std::size_t { return frequency_domains_; }
    [[nodiscard]] auto frequency(std::size_t index) const noexcept -> std::optional<MonitoringPreparationFrequencyView> {
        if (index >= frequency_domains_) return std::nullopt;
        return frequency_(backing_, index);
    }
  private:
    using FrequencyAccessor = MonitoringPreparationFrequencyView (*)(const void*,std::size_t) noexcept;
    static auto project(MonitoringPreparationFactDomain, const MonitoringPreparationCheckpoint&,
        bool retired, const PairwiseMonitoringHostCapacityAdmission&,
        std::span<const monitoring::MonitoringWorkerScope>, std::size_t domains,
        const void*, FrequencyAccessor) noexcept -> std::optional<MonitoringPreparationFactView>;
    MonitoringPreparationFactView(MonitoringPreparationFactDomain domain,
        const MonitoringPreparationCheckpoint& checkpoint,
        const PairwiseMonitoringHostCapacityAdmission& admission,
        std::span<const monitoring::MonitoringWorkerScope> workers, std::size_t domains,
        const void* backing, FrequencyAccessor accessor) noexcept
        : domain_{domain}, checkpoint_{&checkpoint}, admission_{&admission}, workers_{workers},
          frequency_domains_{domains}, backing_{backing}, frequency_{accessor} {}
    MonitoringPreparationFactDomain domain_;
    const MonitoringPreparationCheckpoint* checkpoint_;
    const PairwiseMonitoringHostCapacityAdmission* admission_;
    std::span<const monitoring::MonitoringWorkerScope> workers_;
    std::size_t frequency_domains_;
    const void* backing_;
    FrequencyAccessor frequency_;
    friend class PairwiseMonitoringOwningFacade;
    friend struct MonitoringPreparationFactTestAccess;
};
#if defined(EPYCBENCH_MONITORING_FACT_TEST)
// Test-target-only lifecycle recorder. It cannot mint a source-domain view or capability.
struct MonitoringPreparationFactTestAccess final {
    MonitoringPreparationCheckpoint checkpoint;
    bool retired{};
    bool failed{};
    auto placed(core::LinuxCpuId assigned, core::LinuxCpuId singleton, core::LinuxCpuId observed) noexcept -> bool {
        return !retired && !failed && checkpoint.service_placed(assigned,singleton,observed);
    }
    auto acquired(const monitoring::MonitoringExecutionPolicy& policy, std::size_t workers,
        std::size_t pages, std::size_t contexts, bool temperature, bool frequency, std::size_t domains) noexcept -> bool {
        return !retired && !failed && checkpoint.complete(policy,workers,pages,contexts,temperature,frequency,domains);
    }
    void failure() noexcept { failed=true; }
    void retire() noexcept { retired=true; }
    auto view(const PairwiseMonitoringHostCapacityAdmission& admission,
        std::span<const monitoring::MonitoringWorkerScope> workers,
        std::span<const MonitoringPreparationFrequencyView> frequency) const noexcept
        -> std::optional<MonitoringPreparationFactView> {
        return MonitoringPreparationFactView::project(MonitoringPreparationFactDomain::synthetic,
            checkpoint,retired,admission,workers,frequency.size(),frequency.data(),
            [](const void* data,std::size_t index) noexcept {
                return static_cast<const MonitoringPreparationFrequencyView*>(data)[index];
            });
    }
};
#endif
} // namespace epycbench::runtime::detail
