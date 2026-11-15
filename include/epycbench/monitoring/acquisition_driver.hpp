#pragma once

#include "epycbench/monitoring/context_switches.hpp"
#include "epycbench/monitoring/frequency.hpp"
#include "epycbench/monitoring/page_faults.hpp"
#include "epycbench/monitoring/temperature.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::monitoring {

namespace detail {
struct MonitoringCaptureRegistration;
struct AcquisitionDriverAccess;
class PairwisePreparedCaptureStorageAccess;
struct PreparedEvidenceHeaderAccess;
}

enum class MonitoringChannelDisposition { disabled, optional, required };
enum class OptionalUnavailableSourcePolicy { fail, warn, disable };
enum class MonitoringEffectiveChannelDisposition { active, unsupported_warning, disabled };
enum class MonitoringSchedulerWaitResult { deadline, interrupted };

class MonitoringExecutionPolicy {
  public:
    [[nodiscard]] static auto create(
        MonitoringChannelDisposition page_faults,
        MonitoringChannelDisposition context_switches,
        MonitoringChannelDisposition temperature,
        MonitoringChannelDisposition frequency, bool before_observation,
        bool after_observation, bool during_observation,
        std::optional<core::Duration> during_interval,
        OptionalUnavailableSourcePolicy optional_unsupported)
        -> std::expected<MonitoringExecutionPolicy, core::Error>;

    [[nodiscard]] auto disposition(MonitoringChannel channel) const noexcept
        -> MonitoringChannelDisposition;
    [[nodiscard]] auto before_observation() const noexcept -> bool;
    [[nodiscard]] auto after_observation() const noexcept -> bool;
    [[nodiscard]] auto during_observation() const noexcept -> bool;
    [[nodiscard]] auto during_interval() const noexcept -> const std::optional<core::Duration>&;
    [[nodiscard]] auto optional_unsupported() const noexcept
        -> OptionalUnavailableSourcePolicy;
    auto operator==(const MonitoringExecutionPolicy&) const noexcept -> bool = default;

  private:
    friend struct detail::PreparedEvidenceHeaderAccess;
    MonitoringExecutionPolicy(std::array<MonitoringChannelDisposition, 4>, bool, bool, bool,
                              std::optional<core::Duration>,
                              OptionalUnavailableSourcePolicy) noexcept;
    std::array<MonitoringChannelDisposition, 4> dispositions_;
    bool before_;
    bool after_;
    bool during_;
    std::optional<core::Duration> interval_;
    OptionalUnavailableSourcePolicy optional_unsupported_;
};

class MonitoringWorkerScope {
  public:
    [[nodiscard]] static auto create(core::WorkerId, platform_linux::LinuxThreadId,
                                     core::LinuxCpuId) noexcept -> MonitoringWorkerScope;
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto linux_cpu() const noexcept -> core::LinuxCpuId;
    [[nodiscard]] auto operational_thread_id() const noexcept
        -> platform_linux::LinuxThreadId;
    auto operator==(const MonitoringWorkerScope&) const noexcept -> bool;

  private:
    MonitoringWorkerScope(core::WorkerId, platform_linux::LinuxThreadId,
                          core::LinuxCpuId) noexcept;
    core::WorkerId worker_id_;
    platform_linux::LinuxThreadId thread_id_;
    core::LinuxCpuId cpu_;
    friend struct detail_MonitoringDriverAccess;
};

struct MonitoringChannelPreflight {
    MonitoringChannel channel;
    MonitoringSourceSupport support;
};

struct MonitoringFrequencyDomain {
    FrequencyPolicyMetadata metadata;
    MonitoringImplementationId implementation_id;
};

class MonitoringSourcePreparationRequest {
  public:
    [[nodiscard]] static auto create(const MonitoringExecutionPolicy& policy,
                                     std::span<const MonitoringWorkerScope> workers) noexcept
        -> MonitoringSourcePreparationRequest;
    [[nodiscard]] auto policy() const noexcept -> const MonitoringExecutionPolicy&;
    [[nodiscard]] auto workers() const noexcept -> std::span<const MonitoringWorkerScope>;

  private:
    MonitoringSourcePreparationRequest(const MonitoringExecutionPolicy&,
                                       std::span<const MonitoringWorkerScope>) noexcept;
    const MonitoringExecutionPolicy* policy_;
    std::span<const MonitoringWorkerScope> workers_;
};

struct MonitoringPreparedSources {
    std::vector<MonitoringFrequencyDomain> frequency_domains;
};

struct MonitoringDriverOperations {
    void* context{};
    std::expected<MonitoringPreparedSources, core::Error> (*prepare_sources)(
        void*, const MonitoringSourcePreparationRequest&) noexcept{};
    std::expected<void, core::Error> (*bind_service_cpu)(void*, core::LinuxCpuId) noexcept{};
    std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*now)(void*) noexcept{};
    std::expected<MonitoringSchedulerWaitResult, core::Error> (*wait_until)(
        void*, platform_linux::RawMonotonicTimestamp) noexcept{};
    void (*wake)(void*) noexcept{};
    MonitoringSourceSupport (*support)(void*, MonitoringChannel) noexcept{};
    std::expected<PageFaultSnapshot, core::Error> (*read_page_fault)(
        void*, core::WorkerId, platform_linux::LinuxThreadId) noexcept{};
    std::expected<ContextSwitchEventBatch, core::Error> (*drain_context_switch)(
        void*, core::WorkerId, platform_linux::LinuxThreadId) noexcept{};
    std::expected<TemperatureSnapshot, core::Error> (*read_temperature)(void*) noexcept{};
    std::expected<FrequencySnapshot, core::Error> (*read_frequency)(
        void*, const FrequencyDomainId&) noexcept{};
};

struct MonitoringChannelRunDisposition {
    MonitoringChannel channel;
    MonitoringChannelDisposition authored;
    MonitoringEffectiveChannelDisposition effective;
    auto operator==(const MonitoringChannelRunDisposition&) const noexcept -> bool = default;
};

class MonitoringDriverActivityEvidence {
  public:
    [[nodiscard]] auto scheduled_rounds() const noexcept -> std::uint64_t;
    [[nodiscard]] auto completed_rounds() const noexcept -> std::uint64_t;
    [[nodiscard]] auto missed_rounds() const noexcept -> std::uint64_t;
    [[nodiscard]] auto service_wakeups() const noexcept -> std::uint64_t;
    [[nodiscard]] auto context_drains() const noexcept -> std::uint64_t;
    [[nodiscard]] auto temperature_attempts() const noexcept -> std::uint64_t;
    [[nodiscard]] auto frequency_attempts() const noexcept -> std::uint64_t;
    auto operator==(const MonitoringDriverActivityEvidence&) const noexcept -> bool = default;

  public: // Driver-owned construction state; no host-private values are represented.
    std::uint64_t scheduled_{};
    std::uint64_t completed_{};
    std::uint64_t missed_{};
    std::uint64_t wakeups_{};
    std::uint64_t drains_{};
    std::uint64_t temperature_{};
    std::uint64_t frequency_{};
    friend class MonitoringObservationCapture;
    friend class detail::PairwisePreparedCaptureStorageAccess;
    friend struct detail_MonitoringDriverAccess;
};

class MonitoringRawCaptureEvidence {
  public:
    MonitoringRawCaptureEvidence() = default;
    MonitoringRawCaptureEvidence(const MonitoringRawCaptureEvidence&);
    auto operator=(const MonitoringRawCaptureEvidence&) -> MonitoringRawCaptureEvidence&;
    MonitoringRawCaptureEvidence(MonitoringRawCaptureEvidence&&) noexcept = default;
    auto operator=(MonitoringRawCaptureEvidence&&) noexcept -> MonitoringRawCaptureEvidence& = default;
    using PageAcquisition = TimestampedMonitoringAcquisition<PageFaultSnapshot>;
    using ContextAcquisition = TimestampedMonitoringAcquisition<ContextSwitchEventBatch>;
    using TemperatureAcquisition = TimestampedMonitoringAcquisition<TemperatureSnapshot>;
    [[nodiscard]] auto page_fault_before() const noexcept -> std::span<const PageAcquisition>;
    [[nodiscard]] auto page_fault_after() const noexcept -> std::span<const PageAcquisition>;
    [[nodiscard]] auto context_switches() const noexcept -> std::span<const ContextAcquisition>;
    [[nodiscard]] auto temperature() const noexcept -> std::span<const TemperatureAcquisition>;
    [[nodiscard]] auto frequency() const noexcept
        -> std::span<const FrequencyDomainAcquisitions>;
    [[nodiscard]] auto frequency_metadata() const noexcept
        -> std::span<const FrequencyPolicyMetadata>;

  public: // Driver-owned storage used to construct immutable capture results.
    std::vector<PageAcquisition> page_before_;
    std::vector<PageAcquisition> page_after_;
    std::vector<ContextAcquisition> context_;
    std::optional<std::size_t> context_count_;
    std::vector<std::size_t> context_worker_offsets_;
    std::vector<TemperatureAcquisition> temperature_;
    std::vector<std::vector<FrequencyAcquisition>> frequency_storage_;
    std::vector<FrequencyDomainAcquisitions> frequency_views_;
    std::vector<FrequencyPolicyMetadata> frequency_metadata_;
    friend class MonitoringObservationCapture;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class MonitoringObservationEvidence {
  public:
    [[nodiscard]] auto page_faults_state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto context_switches_state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto temperature_state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto frequency_state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] constexpr auto throttling_state() const noexcept -> MonitoringSignalState {
        return MonitoringSignalState::not_applicable;
    }
    [[nodiscard]] auto page_faults() const noexcept
        -> const std::optional<PageFaultObservationEvidence>&;
    [[nodiscard]] auto context_switches() const noexcept
        -> const std::optional<ContextSwitchObservationEvidence>&;
    [[nodiscard]] auto temperature() const noexcept
        -> const std::optional<TemperatureObservationEvidence>&;
    [[nodiscard]] auto frequency() const noexcept
        -> const std::optional<FrequencyObservationEvidence>&;

  public: // Driver-owned construction storage.
    std::array<MonitoringSignalState, 4> states_{};
    std::optional<PageFaultObservationEvidence> page_;
    std::optional<ContextSwitchObservationEvidence> context_;
    std::optional<TemperatureObservationEvidence> temperature_;
    std::optional<FrequencyObservationEvidence> frequency_;
    friend class MonitoringObservationCapture;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class MonitoringCaptureResult {
  public:
    [[nodiscard]] auto evidence() const noexcept -> const MonitoringObservationEvidence&;
    [[nodiscard]] auto raw() const noexcept -> const MonitoringRawCaptureEvidence&;
    [[nodiscard]] auto activity() const noexcept -> const MonitoringDriverActivityEvidence&;
  public: // Driver-owned construction storage.
    MonitoringObservationEvidence evidence_;
    MonitoringRawCaptureEvidence raw_;
    MonitoringDriverActivityEvidence activity_;
    friend class MonitoringObservationCapture;
};

class MonitoringObservationCapture;

class MonitoringAcquisitionDriver {
  public:
    struct State;
    [[nodiscard]] static auto create(MonitoringExecutionPolicy,
                                     std::vector<MonitoringWorkerScope>,
                                     std::optional<core::LinuxCpuId>, MonitoringDriverOperations)
        -> std::expected<MonitoringAcquisitionDriver, core::Error>;
    [[nodiscard]] auto begin_capture()
        -> std::expected<MonitoringObservationCapture, core::Error>;
    [[nodiscard]] auto stop_and_join() -> std::expected<void, core::Error>;
    [[nodiscard]] auto dispositions() const noexcept
        -> std::span<const MonitoringChannelRunDisposition>;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto has_service_thread() const noexcept -> bool;

    MonitoringAcquisitionDriver(MonitoringAcquisitionDriver&&) noexcept;
    MonitoringAcquisitionDriver(const MonitoringAcquisitionDriver&) = delete;
    auto operator=(const MonitoringAcquisitionDriver&) -> MonitoringAcquisitionDriver& = delete;
    auto operator=(MonitoringAcquisitionDriver&&) -> MonitoringAcquisitionDriver& = delete;
    ~MonitoringAcquisitionDriver() noexcept;

  private:
    explicit MonitoringAcquisitionDriver(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class MonitoringObservationCapture;
    friend struct detail::AcquisitionDriverAccess;
};

class MonitoringObservationCapture {
  public:
    [[nodiscard]] auto complete(platform_linux::RawMonotonicTimestamp,
                                platform_linux::RawMonotonicTimestamp)
        -> std::expected<MonitoringCaptureResult, core::Error>;
    [[nodiscard]] auto abort() -> std::expected<void, core::Error>;
    [[nodiscard]] auto generation() const noexcept -> std::uint64_t;
    MonitoringObservationCapture(MonitoringObservationCapture&&) noexcept;
    MonitoringObservationCapture(const MonitoringObservationCapture&) = delete;
    auto operator=(const MonitoringObservationCapture&) -> MonitoringObservationCapture& = delete;
    auto operator=(MonitoringObservationCapture&&) -> MonitoringObservationCapture& = delete;
    ~MonitoringObservationCapture() noexcept;
  private:
    MonitoringObservationCapture(MonitoringAcquisitionDriver::State*, std::uint64_t,
                                 std::shared_ptr<detail::MonitoringCaptureRegistration>) noexcept;
    MonitoringAcquisitionDriver::State* state_{};
    std::shared_ptr<detail::MonitoringCaptureRegistration> registration_;
    std::uint64_t generation_{};
    bool terminal_{};
    friend class MonitoringAcquisitionDriver;
    friend struct detail::AcquisitionDriverAccess;
};

} // namespace epycbench::monitoring
