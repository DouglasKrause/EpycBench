#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/monitoring/monitor_source.hpp"
#include "epycbench/platform_linux/task_stat.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace epycbench::monitoring {

namespace detail {
class PageFaultSourceAccess;
class PairwisePreparedCaptureStorageAccess;
}

class PageFaultCount {
  public:
    [[nodiscard]] static constexpr auto from_events(std::uint64_t value) noexcept
        -> PageFaultCount {
        return PageFaultCount{value};
    }
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return value_; }

    constexpr auto operator<=>(const PageFaultCount&) const noexcept = default;

  private:
    constexpr explicit PageFaultCount(std::uint64_t value) noexcept : value_{value} {}
    std::uint64_t value_;
};

class PageFaultSnapshot {
  public:
    [[nodiscard]] static constexpr auto create(core::WorkerId worker_id,
                                               PageFaultCount minor_faults,
                                               PageFaultCount major_faults) noexcept
        -> PageFaultSnapshot {
        return PageFaultSnapshot{worker_id, minor_faults, major_faults};
    }

    [[nodiscard]] constexpr auto worker_id() const noexcept -> core::WorkerId { return worker_id_; }
    [[nodiscard]] constexpr auto minor_faults() const noexcept -> PageFaultCount {
        return minor_faults_;
    }
    [[nodiscard]] constexpr auto major_faults() const noexcept -> PageFaultCount {
        return major_faults_;
    }

    constexpr auto operator<=>(const PageFaultSnapshot&) const noexcept = default;

  private:
    constexpr PageFaultSnapshot(core::WorkerId worker_id, PageFaultCount minor_faults,
                                PageFaultCount major_faults) noexcept
        : worker_id_{worker_id}, minor_faults_{minor_faults}, major_faults_{major_faults} {}

    core::WorkerId worker_id_;
    PageFaultCount minor_faults_;
    PageFaultCount major_faults_;
};

class PageFaultSource {
  public:
    [[nodiscard]] static auto create(core::WorkerId worker_id,
                                     platform_linux::LinuxThreadId thread_id) noexcept
        -> PageFaultSource;

    [[nodiscard]] auto operations() noexcept
        -> std::expected<MonitoringSourceOperations<PageFaultSnapshot>, core::Error>;
    [[nodiscard]] static auto implementation_id() -> MonitoringImplementationId;

    PageFaultSource(const PageFaultSource&) = delete;
    auto operator=(const PageFaultSource&) -> PageFaultSource& = delete;
    PageFaultSource(PageFaultSource&&) noexcept = default;
    auto operator=(PageFaultSource&&) -> PageFaultSource& = delete;
    ~PageFaultSource() noexcept = default;

  private:
    using ReadOperation =
        std::expected<platform_linux::LinuxTaskPageFaultCounters, core::Error> (*)(
            void*, platform_linux::LinuxThreadId) noexcept;

    PageFaultSource(core::WorkerId worker_id, platform_linux::LinuxThreadId thread_id,
                    void* reader_context, ReadOperation read_operation) noexcept;

    static auto support_callback(void*) noexcept -> MonitoringSourceSupport;
    static auto read_callback(void* context) noexcept
        -> std::expected<MonitoringPayloadResult<PageFaultSnapshot>, core::Error>;

    core::WorkerId worker_id_;
    platform_linux::LinuxThreadId thread_id_;
    void* reader_context_;
    ReadOperation read_operation_;

    friend class detail::PageFaultSourceAccess;
};

class WorkerPageFaultEvidence {
  public:
    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto minor_delta() const noexcept -> const std::optional<PageFaultCount>&;
    [[nodiscard]] auto major_delta() const noexcept -> const std::optional<PageFaultCount>&;

    auto operator==(const WorkerPageFaultEvidence&) const noexcept -> bool = default;

  private:
    WorkerPageFaultEvidence(core::WorkerId worker_id, MonitoringSignalState state,
                            std::optional<PageFaultCount> minor_delta,
                            std::optional<PageFaultCount> major_delta) noexcept;

    core::WorkerId worker_id_;
    MonitoringSignalState state_;
    std::optional<PageFaultCount> minor_delta_;
    std::optional<PageFaultCount> major_delta_;

    friend class PageFaultObservationEvidence;
    friend class detail::PairwisePreparedCaptureStorageAccess;
    friend auto classify_page_faults(platform_linux::RawMonotonicTimestamp,
                                     platform_linux::RawMonotonicTimestamp,
                                     std::span<const class PageFaultAcquisitionPair>)
        -> std::expected<class PageFaultObservationEvidence, core::Error>;
};

class PageFaultAcquisitionPair {
  public:
    using Acquisition = TimestampedMonitoringAcquisition<PageFaultSnapshot>;

    PageFaultAcquisitionPair(core::WorkerId worker_id, const Acquisition& before,
                             const Acquisition& after) noexcept
        : worker_id_{worker_id}, before_{&before}, after_{&after} {}

    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId { return worker_id_; }
    [[nodiscard]] auto before() const noexcept -> const Acquisition& { return *before_; }
    [[nodiscard]] auto after() const noexcept -> const Acquisition& { return *after_; }

  private:
    core::WorkerId worker_id_;
    const Acquisition* before_;
    const Acquisition* after_;
};

class PageFaultObservationEvidence {
  public:
    [[nodiscard]] auto state() const noexcept -> MonitoringSignalState;
    [[nodiscard]] auto workers() const noexcept -> std::span<const WorkerPageFaultEvidence>;

    auto operator==(const PageFaultObservationEvidence&) const noexcept -> bool = default;

  private:
    PageFaultObservationEvidence(MonitoringSignalState state,
                                 std::vector<WorkerPageFaultEvidence> workers) noexcept;

    MonitoringSignalState state_;
    std::vector<WorkerPageFaultEvidence> workers_;

    friend auto classify_page_faults(platform_linux::RawMonotonicTimestamp,
                                     platform_linux::RawMonotonicTimestamp,
                                     std::span<const PageFaultAcquisitionPair>)
        -> std::expected<PageFaultObservationEvidence, core::Error>;
    friend class detail::PairwisePreparedCaptureStorageAccess;
};

[[nodiscard]] auto classify_page_faults(platform_linux::RawMonotonicTimestamp observation_start,
                                        platform_linux::RawMonotonicTimestamp observation_end,
                                        std::span<const PageFaultAcquisitionPair> workers)
    -> std::expected<PageFaultObservationEvidence, core::Error>;

} // namespace epycbench::monitoring
