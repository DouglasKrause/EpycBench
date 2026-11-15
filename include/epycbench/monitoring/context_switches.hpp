#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/monitoring/monitor_source.hpp"
#include "epycbench/platform_linux/perf_context_switch.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::monitoring {
namespace detail {
class ContextSwitchSourceAccess;
class PairwisePreparedCaptureStorageAccess;
}

enum class ContextSwitchKind { voluntary, involuntary };
enum class ContextSwitchCaptureCompleteness { complete, incomplete };

class ContextSwitchCount {
  public:
    [[nodiscard]] static constexpr auto from_events(std::uint64_t value) noexcept
        -> ContextSwitchCount {
        return ContextSwitchCount{value};
    }
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }
    constexpr auto operator<=>(const ContextSwitchCount&) const noexcept = default;

  private:
    constexpr explicit ContextSwitchCount(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class ContextSwitchEvent {
  public:
    [[nodiscard]] static constexpr auto create(core::WorkerId worker_id,
                                               platform_linux::RawMonotonicTimestamp timestamp,
                                               ContextSwitchKind kind) noexcept
        -> ContextSwitchEvent {
        return ContextSwitchEvent{worker_id, timestamp, kind};
    }
    [[nodiscard]] constexpr auto worker_id() const noexcept -> core::WorkerId { return worker_id_; }
    [[nodiscard]] constexpr auto timestamp() const noexcept
        -> platform_linux::RawMonotonicTimestamp {
        return timestamp_;
    }
    [[nodiscard]] constexpr auto kind() const noexcept -> ContextSwitchKind { return kind_; }
    constexpr auto operator==(const ContextSwitchEvent&) const noexcept -> bool = default;

  private:
    constexpr ContextSwitchEvent(core::WorkerId worker_id,
                                 platform_linux::RawMonotonicTimestamp timestamp,
                                 ContextSwitchKind kind) noexcept
        : worker_id_{worker_id}, timestamp_{timestamp}, kind_{kind} {}
    core::WorkerId worker_id_;
    platform_linux::RawMonotonicTimestamp timestamp_;
    ContextSwitchKind kind_;
};

class ContextSwitchEventBatch {
  public:
    [[nodiscard]] static auto create(core::WorkerId worker_id,
                                     ContextSwitchCaptureCompleteness completeness,
                                     std::vector<ContextSwitchEvent> events)
        -> ContextSwitchEventBatch;
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto completeness() const noexcept -> ContextSwitchCaptureCompleteness;
    [[nodiscard]] auto events() const noexcept -> std::span<const ContextSwitchEvent>;
    auto operator==(const ContextSwitchEventBatch&) const noexcept -> bool = default;

  private:
    ContextSwitchEventBatch(core::WorkerId worker_id, ContextSwitchCaptureCompleteness completeness,
                            std::vector<ContextSwitchEvent> events) noexcept;
    core::WorkerId worker_id_;
    ContextSwitchCaptureCompleteness completeness_;
    std::vector<ContextSwitchEvent> events_;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

class ContextSwitchEventSource {
  public:
    [[nodiscard]] static auto prepare(core::WorkerId worker_id,
                                      platform_linux::LinuxThreadId thread_id)
        -> std::expected<ContextSwitchEventSource, core::Error>;
    [[nodiscard]] auto operations() noexcept
        -> std::expected<MonitoringSourceOperations<ContextSwitchEventBatch>, core::Error>;
    [[nodiscard]] static auto implementation_id() -> MonitoringImplementationId;

    ContextSwitchEventSource(ContextSwitchEventSource&&) noexcept;
    ContextSwitchEventSource(const ContextSwitchEventSource&) = delete;
    auto operator=(const ContextSwitchEventSource&) -> ContextSwitchEventSource& = delete;
    auto operator=(ContextSwitchEventSource&&) -> ContextSwitchEventSource& = delete;
    ~ContextSwitchEventSource() noexcept;

  private:
    using DrainOperation =
        std::expected<platform_linux::PerfContextSwitchDrain, core::Error> (*)(void*) noexcept;
    ContextSwitchEventSource(
        core::WorkerId worker_id, void* context, DrainOperation drain,
        std::unique_ptr<platform_linux::PerfContextSwitchSource> source) noexcept;
    static auto support_callback(void*) noexcept -> MonitoringSourceSupport;
    static auto drain_callback(void*) noexcept
        -> std::expected<MonitoringPayloadResult<ContextSwitchEventBatch>, core::Error>;

    core::WorkerId worker_id_;
    void* context_{};
    DrainOperation drain_{};
    std::unique_ptr<platform_linux::PerfContextSwitchSource> source_;
    friend class detail::ContextSwitchSourceAccess;
};

class ContextSwitchWorkerAcquisitionView {
  public:
    using Acquisition = TimestampedMonitoringAcquisition<ContextSwitchEventBatch>;
    ContextSwitchWorkerAcquisitionView(core::WorkerId worker_id,
                                       MonitoringImplementationId implementation_id,
                                       platform_linux::RawMonotonicTimestamp coverage_start,
                                       platform_linux::RawMonotonicTimestamp coverage_end,
                                       ContextSwitchCaptureCompleteness completeness,
                                       std::span<const Acquisition> acquisitions) noexcept;
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto implementation_id() const noexcept -> const MonitoringImplementationId&;
    [[nodiscard]] auto coverage_start() const noexcept -> platform_linux::RawMonotonicTimestamp;
    [[nodiscard]] auto coverage_end() const noexcept -> platform_linux::RawMonotonicTimestamp;
    [[nodiscard]] auto completeness() const noexcept -> ContextSwitchCaptureCompleteness;
    [[nodiscard]] auto acquisitions() const noexcept -> std::span<const Acquisition>;

  private:
    core::WorkerId worker_id_;
    MonitoringImplementationId implementation_id_;
    platform_linux::RawMonotonicTimestamp coverage_start_;
    platform_linux::RawMonotonicTimestamp coverage_end_;
    ContextSwitchCaptureCompleteness completeness_;
    std::span<const Acquisition> acquisitions_;
};

class WorkerContextSwitchEvidence {
  public:
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto voluntary_count() const noexcept -> const std::optional<ContextSwitchCount>&;
    [[nodiscard]] auto involuntary_count() const noexcept
        -> const std::optional<ContextSwitchCount>&;
    auto operator==(const WorkerContextSwitchEvidence&) const noexcept -> bool = default;

  private:
    WorkerContextSwitchEvidence(core::WorkerId worker_id, MonitoringSignalState state,
                                std::optional<ContextSwitchCount> voluntary,
                                std::optional<ContextSwitchCount> involuntary) noexcept;
    core::WorkerId worker_id_;
    MonitoringSignalState state_;
    std::optional<ContextSwitchCount> voluntary_;
    std::optional<ContextSwitchCount> involuntary_;
    friend class ContextSwitchObservationEvidence;
    friend class detail::PairwisePreparedCaptureStorageAccess;
    friend auto classify_context_switches(platform_linux::RawMonotonicTimestamp,
                                          platform_linux::RawMonotonicTimestamp,
                                          std::span<const core::WorkerId>,
                                          std::span<const ContextSwitchWorkerAcquisitionView>)
        -> std::expected<class ContextSwitchObservationEvidence, core::Error>;
};

class ContextSwitchObservationEvidence {
  public:
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto workers() const noexcept -> std::span<const WorkerContextSwitchEvidence>;
    auto operator==(const ContextSwitchObservationEvidence&) const noexcept -> bool = default;

  private:
    ContextSwitchObservationEvidence(MonitoringSignalState state,
                                     std::vector<WorkerContextSwitchEvidence> workers) noexcept;
    MonitoringSignalState state_;
    std::vector<WorkerContextSwitchEvidence> workers_;
    friend auto classify_context_switches(platform_linux::RawMonotonicTimestamp,
                                          platform_linux::RawMonotonicTimestamp,
                                          std::span<const core::WorkerId>,
                                          std::span<const ContextSwitchWorkerAcquisitionView>)
        -> std::expected<ContextSwitchObservationEvidence, core::Error>;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

[[nodiscard]] auto
classify_context_switches(platform_linux::RawMonotonicTimestamp observation_start,
                          platform_linux::RawMonotonicTimestamp observation_end,
                          std::span<const core::WorkerId> canonical_workers,
                          std::span<const ContextSwitchWorkerAcquisitionView> worker_acquisitions)
    -> std::expected<ContextSwitchObservationEvidence, core::Error>;

} // namespace epycbench::monitoring
