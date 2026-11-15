#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/kernel/configuration.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/descriptor.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include <expected>
#include <span>
#include <variant>
#include <vector>

namespace epycbench::measurement {

namespace detail {
struct MeasurementLeafAccess;
}

class MeasurementInterval {
  public:
    MeasurementInterval(const MeasurementInterval&) noexcept = default;
    MeasurementInterval(MeasurementInterval&&) noexcept = default;
    auto operator=(const MeasurementInterval&) -> MeasurementInterval& = delete;
    auto operator=(MeasurementInterval&&) -> MeasurementInterval& = delete;
    ~MeasurementInterval() noexcept = default;

    [[nodiscard]] static auto create(platform_linux::RawMonotonicTimestamp start,
                                     platform_linux::RawMonotonicTimestamp end)
        -> std::expected<MeasurementInterval, core::Error>;

    [[nodiscard]] auto start() const noexcept -> platform_linux::RawMonotonicTimestamp;
    [[nodiscard]] auto end() const noexcept -> platform_linux::RawMonotonicTimestamp;
    [[nodiscard]] auto elapsed() const noexcept -> core::Duration;

  private:
    friend struct detail::MeasurementLeafAccess;
    MeasurementInterval(platform_linux::RawMonotonicTimestamp start,
                        platform_linux::RawMonotonicTimestamp end,
                        core::Duration elapsed) noexcept;

    const platform_linux::RawMonotonicTimestamp start_;
    const platform_linux::RawMonotonicTimestamp end_;
    const core::Duration elapsed_;
};

class SequentialReadBandwidthWorkerObservation {
  public:
    SequentialReadBandwidthWorkerObservation(
        const SequentialReadBandwidthWorkerObservation&) noexcept = default;
    SequentialReadBandwidthWorkerObservation(
        SequentialReadBandwidthWorkerObservation&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthWorkerObservation&)
        -> SequentialReadBandwidthWorkerObservation& = delete;
    auto operator=(SequentialReadBandwidthWorkerObservation&&)
        -> SequentialReadBandwidthWorkerObservation& = delete;
    ~SequentialReadBandwidthWorkerObservation() noexcept = default;

    [[nodiscard]] static auto create(
        core::WorkerId worker_id, MeasurementInterval interval,
        kernel::SequentialReadBandwidthWork work,
        kernel::SequentialReadBandwidthVerification verification) noexcept
        -> SequentialReadBandwidthWorkerObservation;

    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto interval() const noexcept -> const MeasurementInterval&;
    [[nodiscard]] auto work() const noexcept -> kernel::SequentialReadBandwidthWork;
    [[nodiscard]] auto verification() const noexcept
        -> kernel::SequentialReadBandwidthVerification;

  private:
    SequentialReadBandwidthWorkerObservation(
        core::WorkerId worker_id, MeasurementInterval interval,
        kernel::SequentialReadBandwidthWork work,
        kernel::SequentialReadBandwidthVerification verification) noexcept;

    const core::WorkerId worker_id_;
    const MeasurementInterval interval_;
    const kernel::SequentialReadBandwidthWork work_;
    const kernel::SequentialReadBandwidthVerification verification_;
};

class DependentLoadLatencyWorkerObservation {
  public:
    DependentLoadLatencyWorkerObservation(
        const DependentLoadLatencyWorkerObservation&) noexcept = default;
    DependentLoadLatencyWorkerObservation(
        DependentLoadLatencyWorkerObservation&&) noexcept = default;
    auto operator=(const DependentLoadLatencyWorkerObservation&)
        -> DependentLoadLatencyWorkerObservation& = delete;
    auto operator=(DependentLoadLatencyWorkerObservation&&)
        -> DependentLoadLatencyWorkerObservation& = delete;
    ~DependentLoadLatencyWorkerObservation() noexcept = default;

    [[nodiscard]] static auto create(
        core::WorkerId worker_id, MeasurementInterval interval,
        kernel::DependentLoadLatencySchedule schedule,
        kernel::DependentLoadLatencyVerification verification) noexcept
        -> DependentLoadLatencyWorkerObservation;

    [[nodiscard]] auto worker_id() const noexcept -> core::WorkerId;
    [[nodiscard]] auto interval() const noexcept -> const MeasurementInterval&;
    [[nodiscard]] auto schedule() const noexcept -> kernel::DependentLoadLatencySchedule;
    [[nodiscard]] auto completed_work() const noexcept
        -> kernel::DependentLoadLatencyStepCount;
    [[nodiscard]] auto verification() const noexcept
        -> kernel::DependentLoadLatencyVerification;

  private:
    DependentLoadLatencyWorkerObservation(
        core::WorkerId worker_id, MeasurementInterval interval,
        kernel::DependentLoadLatencySchedule schedule,
        kernel::DependentLoadLatencyVerification verification) noexcept;

    const core::WorkerId worker_id_;
    const MeasurementInterval interval_;
    const kernel::DependentLoadLatencySchedule schedule_;
    const kernel::DependentLoadLatencyVerification verification_;
};

class SequentialReadBandwidthObservation {
  public:
    SequentialReadBandwidthObservation(const SequentialReadBandwidthObservation&) = default;
    SequentialReadBandwidthObservation(SequentialReadBandwidthObservation&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthObservation&)
        -> SequentialReadBandwidthObservation& = delete;
    auto operator=(SequentialReadBandwidthObservation&&)
        -> SequentialReadBandwidthObservation& = delete;
    ~SequentialReadBandwidthObservation() noexcept = default;

    [[nodiscard]] static auto create(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::SequentialReadBandwidthConfig configuration,
        kernel::SequentialReadBandwidthPassCount pass_count,
        std::vector<SequentialReadBandwidthWorkerObservation> workers,
        MeasurementInterval enclosing_interval)
        -> std::expected<SequentialReadBandwidthObservation, core::Error>;

    [[nodiscard]] auto observation_id() const noexcept -> core::ObservationId;
    [[nodiscard]] auto experiment_id() const noexcept -> core::ExperimentId;
    [[nodiscard]] auto block_id() const noexcept -> kernel::DependentLoadLatencyBlockId;
    [[nodiscard]] auto implementation_id() const noexcept
        -> kernel::KernelImplementationId;
    [[nodiscard]] auto configuration() const noexcept
        -> const kernel::SequentialReadBandwidthConfig&;
    [[nodiscard]] auto pass_count() const noexcept
        -> kernel::SequentialReadBandwidthPassCount;
    [[nodiscard]] auto workers() const noexcept
        -> std::span<const SequentialReadBandwidthWorkerObservation>;
    [[nodiscard]] auto enclosing_interval() const noexcept -> const MeasurementInterval&;
    [[nodiscard]] auto aggregate_work() const noexcept -> core::ByteCount;

  private:
    friend struct detail::MeasurementLeafAccess;
    SequentialReadBandwidthObservation(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::SequentialReadBandwidthConfig configuration,
        kernel::SequentialReadBandwidthPassCount pass_count,
        std::vector<SequentialReadBandwidthWorkerObservation> workers,
        MeasurementInterval enclosing_interval, core::ByteCount aggregate_work) noexcept;

    const core::ObservationId observation_id_;
    const core::ExperimentId experiment_id_;
    const kernel::DependentLoadLatencyBlockId block_id_;
    const kernel::KernelImplementationId implementation_id_;
    const kernel::SequentialReadBandwidthConfig configuration_;
    const kernel::SequentialReadBandwidthPassCount pass_count_;
    std::vector<SequentialReadBandwidthWorkerObservation> workers_;
    const MeasurementInterval enclosing_interval_;
    const core::ByteCount aggregate_work_;
};

class DependentLoadLatencyObservation {
  public:
    DependentLoadLatencyObservation(const DependentLoadLatencyObservation&) = default;
    DependentLoadLatencyObservation(DependentLoadLatencyObservation&&) noexcept = default;
    auto operator=(const DependentLoadLatencyObservation&)
        -> DependentLoadLatencyObservation& = delete;
    auto operator=(DependentLoadLatencyObservation&&)
        -> DependentLoadLatencyObservation& = delete;
    ~DependentLoadLatencyObservation() noexcept = default;

    [[nodiscard]] static auto create(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::DependentLoadLatencyConfig configuration,
        std::vector<DependentLoadLatencyWorkerObservation> workers,
        MeasurementInterval enclosing_interval)
        -> std::expected<DependentLoadLatencyObservation, core::Error>;

    [[nodiscard]] auto observation_id() const noexcept -> core::ObservationId;
    [[nodiscard]] auto experiment_id() const noexcept -> core::ExperimentId;
    [[nodiscard]] auto block_id() const noexcept -> kernel::DependentLoadLatencyBlockId;
    [[nodiscard]] auto implementation_id() const noexcept
        -> kernel::KernelImplementationId;
    [[nodiscard]] auto configuration() const noexcept
        -> const kernel::DependentLoadLatencyConfig&;
    [[nodiscard]] auto workers() const noexcept
        -> std::span<const DependentLoadLatencyWorkerObservation>;
    [[nodiscard]] auto enclosing_interval() const noexcept -> const MeasurementInterval&;
    [[nodiscard]] auto aggregate_work() const noexcept -> core::WorkCount;

  private:
    friend struct detail::MeasurementLeafAccess;
    DependentLoadLatencyObservation(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::DependentLoadLatencyConfig configuration,
        std::vector<DependentLoadLatencyWorkerObservation> workers,
        MeasurementInterval enclosing_interval, core::WorkCount aggregate_work) noexcept;

    const core::ObservationId observation_id_;
    const core::ExperimentId experiment_id_;
    const kernel::DependentLoadLatencyBlockId block_id_;
    const kernel::KernelImplementationId implementation_id_;
    const kernel::DependentLoadLatencyConfig configuration_;
    std::vector<DependentLoadLatencyWorkerObservation> workers_;
    const MeasurementInterval enclosing_interval_;
    const core::WorkCount aggregate_work_;
};

using MeasurementObservation =
    std::variant<SequentialReadBandwidthObservation, DependentLoadLatencyObservation>;

} // namespace epycbench::measurement
