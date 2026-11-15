#pragma once

#include "epycbench/measurement/empty_harness.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"

#include <expected>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace epycbench::measurement {

namespace detail {
struct MeasurementLeafAccess;
}

class CalibrationDurationPolicy {
  public:
    CalibrationDurationPolicy(const CalibrationDurationPolicy&) noexcept = default;
    CalibrationDurationPolicy(CalibrationDurationPolicy&&) noexcept = default;
    auto operator=(const CalibrationDurationPolicy&) -> CalibrationDurationPolicy& = delete;
    auto operator=(CalibrationDurationPolicy&&) -> CalibrationDurationPolicy& = delete;
    ~CalibrationDurationPolicy() noexcept = default;

    [[nodiscard]] static auto create(core::Duration minimum, core::Duration target,
                                     core::Duration maximum)
        -> std::expected<CalibrationDurationPolicy, core::Error>;

    [[nodiscard]] auto minimum_duration() const noexcept -> core::Duration;
    [[nodiscard]] auto target_duration() const noexcept -> core::Duration;
    [[nodiscard]] auto maximum_duration() const noexcept -> core::Duration;

  private:
    friend struct detail::MeasurementLeafAccess;
    CalibrationDurationPolicy(core::Duration minimum, core::Duration target,
                              core::Duration maximum) noexcept;

    const core::Duration minimum_duration_;
    const core::Duration target_duration_;
    const core::Duration maximum_duration_;
};

class TimerOverheadEstimate {
  public:
    TimerOverheadEstimate(const TimerOverheadEstimate&) noexcept = default;
    TimerOverheadEstimate(TimerOverheadEstimate&&) noexcept = default;
    auto operator=(const TimerOverheadEstimate&) -> TimerOverheadEstimate& = delete;
    auto operator=(TimerOverheadEstimate&&) -> TimerOverheadEstimate& = delete;
    ~TimerOverheadEstimate() noexcept = default;

    [[nodiscard]] static auto create(core::Duration duration) noexcept -> TimerOverheadEstimate;
    [[nodiscard]] auto duration() const noexcept -> core::Duration;

  private:
    explicit TimerOverheadEstimate(core::Duration duration) noexcept;
    const core::Duration duration_;
};

class SequentialReadBandwidthCalibrationTrial {
  public:
    SequentialReadBandwidthCalibrationTrial(
        const SequentialReadBandwidthCalibrationTrial&) noexcept = default;
    SequentialReadBandwidthCalibrationTrial(
        SequentialReadBandwidthCalibrationTrial&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthCalibrationTrial&)
        -> SequentialReadBandwidthCalibrationTrial& = delete;
    auto operator=(SequentialReadBandwidthCalibrationTrial&&)
        -> SequentialReadBandwidthCalibrationTrial& = delete;
    ~SequentialReadBandwidthCalibrationTrial() noexcept = default;

    [[nodiscard]] static auto create(kernel::SequentialReadBandwidthPassCount candidate,
                                     MeasurementInterval interval) noexcept
        -> SequentialReadBandwidthCalibrationTrial;
    [[nodiscard]] auto candidate() const noexcept
        -> kernel::SequentialReadBandwidthPassCount;
    [[nodiscard]] auto interval() const noexcept -> const MeasurementInterval&;

  private:
    SequentialReadBandwidthCalibrationTrial(
        kernel::SequentialReadBandwidthPassCount candidate,
        MeasurementInterval interval) noexcept;
    const kernel::SequentialReadBandwidthPassCount candidate_;
    const MeasurementInterval interval_;
};

class DependentLoadLatencyCalibrationTrial {
  public:
    DependentLoadLatencyCalibrationTrial(
        const DependentLoadLatencyCalibrationTrial&) noexcept = default;
    DependentLoadLatencyCalibrationTrial(
        DependentLoadLatencyCalibrationTrial&&) noexcept = default;
    auto operator=(const DependentLoadLatencyCalibrationTrial&)
        -> DependentLoadLatencyCalibrationTrial& = delete;
    auto operator=(DependentLoadLatencyCalibrationTrial&&)
        -> DependentLoadLatencyCalibrationTrial& = delete;
    ~DependentLoadLatencyCalibrationTrial() noexcept = default;

    [[nodiscard]] static auto create(kernel::DependentLoadLatencyStepCount candidate,
                                     MeasurementInterval interval) noexcept
        -> DependentLoadLatencyCalibrationTrial;
    [[nodiscard]] auto candidate() const noexcept -> kernel::DependentLoadLatencyStepCount;
    [[nodiscard]] auto interval() const noexcept -> const MeasurementInterval&;

  private:
    DependentLoadLatencyCalibrationTrial(kernel::DependentLoadLatencyStepCount candidate,
                                         MeasurementInterval interval) noexcept;
    const kernel::DependentLoadLatencyStepCount candidate_;
    const MeasurementInterval interval_;
};

enum class CalibrationDisposition {
    selected,
    minimum_candidate_above_maximum_duration,
    maximum_candidate_below_minimum_duration,
    no_candidate_in_acceptable_band,
};

namespace detail {
class CalibrationAccess;
}

class SequentialReadBandwidthCalibrationOutcome {
  public:
    SequentialReadBandwidthCalibrationOutcome(
        const SequentialReadBandwidthCalibrationOutcome&) = default;
    SequentialReadBandwidthCalibrationOutcome(
        SequentialReadBandwidthCalibrationOutcome&&) noexcept = default;
    auto operator=(const SequentialReadBandwidthCalibrationOutcome&)
        -> SequentialReadBandwidthCalibrationOutcome& = delete;
    auto operator=(SequentialReadBandwidthCalibrationOutcome&&)
        -> SequentialReadBandwidthCalibrationOutcome& = delete;
    ~SequentialReadBandwidthCalibrationOutcome() noexcept = default;

    [[nodiscard]] auto duration_policy() const noexcept -> const CalibrationDurationPolicy&;
    [[nodiscard]] auto empty_harness_trials() const noexcept
        -> std::span<const EmptyHarnessTrial>;
    [[nodiscard]] auto timer_overhead_estimate() const noexcept -> TimerOverheadEstimate;
    [[nodiscard]] auto candidate_trials() const noexcept
        -> std::span<const SequentialReadBandwidthCalibrationTrial>;
    [[nodiscard]] auto disposition() const noexcept -> CalibrationDisposition;
    [[nodiscard]] auto selected_candidate() const noexcept
        -> std::optional<kernel::SequentialReadBandwidthPassCount>;

  private:
    SequentialReadBandwidthCalibrationOutcome(
        CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
        TimerOverheadEstimate estimate,
        std::vector<SequentialReadBandwidthCalibrationTrial> candidate_trials,
        CalibrationDisposition disposition,
        std::optional<kernel::SequentialReadBandwidthPassCount> selected) noexcept;

    const CalibrationDurationPolicy policy_;
    std::vector<EmptyHarnessTrial> empty_trials_;
    const TimerOverheadEstimate estimate_;
    std::vector<SequentialReadBandwidthCalibrationTrial> candidate_trials_;
    const CalibrationDisposition disposition_;
    const std::optional<kernel::SequentialReadBandwidthPassCount> selected_;
    friend class detail::CalibrationAccess;
};

class DependentLoadLatencyCalibrationOutcome {
  public:
    DependentLoadLatencyCalibrationOutcome(
        const DependentLoadLatencyCalibrationOutcome&) = default;
    DependentLoadLatencyCalibrationOutcome(
        DependentLoadLatencyCalibrationOutcome&&) noexcept = default;
    auto operator=(const DependentLoadLatencyCalibrationOutcome&)
        -> DependentLoadLatencyCalibrationOutcome& = delete;
    auto operator=(DependentLoadLatencyCalibrationOutcome&&)
        -> DependentLoadLatencyCalibrationOutcome& = delete;
    ~DependentLoadLatencyCalibrationOutcome() noexcept = default;

    [[nodiscard]] auto duration_policy() const noexcept -> const CalibrationDurationPolicy&;
    [[nodiscard]] auto empty_harness_trials() const noexcept
        -> std::span<const EmptyHarnessTrial>;
    [[nodiscard]] auto timer_overhead_estimate() const noexcept -> TimerOverheadEstimate;
    [[nodiscard]] auto candidate_trials() const noexcept
        -> std::span<const DependentLoadLatencyCalibrationTrial>;
    [[nodiscard]] auto disposition() const noexcept -> CalibrationDisposition;
    [[nodiscard]] auto selected_candidate() const noexcept
        -> std::optional<kernel::DependentLoadLatencyStepCount>;

  private:
    DependentLoadLatencyCalibrationOutcome(
        CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
        TimerOverheadEstimate estimate,
        std::vector<DependentLoadLatencyCalibrationTrial> candidate_trials,
        CalibrationDisposition disposition,
        std::optional<kernel::DependentLoadLatencyStepCount> selected) noexcept;

    const CalibrationDurationPolicy policy_;
    std::vector<EmptyHarnessTrial> empty_trials_;
    const TimerOverheadEstimate estimate_;
    std::vector<DependentLoadLatencyCalibrationTrial> candidate_trials_;
    const CalibrationDisposition disposition_;
    const std::optional<kernel::DependentLoadLatencyStepCount> selected_;
    friend class detail::CalibrationAccess;
};

using CalibrationOutcome = std::variant<SequentialReadBandwidthCalibrationOutcome,
                                        DependentLoadLatencyCalibrationOutcome>;

namespace detail {

class CalibrationAccess {
  public:
    [[nodiscard]] static auto make_outcome(
        CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
        TimerOverheadEstimate estimate,
        std::vector<SequentialReadBandwidthCalibrationTrial> candidate_trials,
        CalibrationDisposition disposition,
        std::optional<kernel::SequentialReadBandwidthPassCount> selected) noexcept
        -> SequentialReadBandwidthCalibrationOutcome;

    [[nodiscard]] static auto make_outcome(
        CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
        TimerOverheadEstimate estimate,
        std::vector<DependentLoadLatencyCalibrationTrial> candidate_trials,
        CalibrationDisposition disposition,
        std::optional<kernel::DependentLoadLatencyStepCount> selected) noexcept
        -> DependentLoadLatencyCalibrationOutcome;
};

struct SequentialReadBandwidthCalibrationOperations {
    void* context;
    std::expected<MeasurementInterval, core::Error> (*run)(
        void*, kernel::SequentialReadBandwidthPassCount) noexcept;
};

struct DependentLoadLatencyCalibrationOperations {
    void* context;
    std::expected<MeasurementInterval, core::Error> (*run)(
        void*, kernel::DependentLoadLatencyStepCount) noexcept;
};

} // namespace detail

[[nodiscard]] auto calibrate_sequential_read_bandwidth(
    CalibrationDurationPolicy policy, const detail::RawMonotonicClockOperations& clock,
    const detail::SequentialReadBandwidthCalibrationOperations& operations)
    -> std::expected<SequentialReadBandwidthCalibrationOutcome, core::Error>;

[[nodiscard]] auto calibrate_dependent_load_latency(
    CalibrationDurationPolicy policy, const detail::RawMonotonicClockOperations& clock,
    const detail::DependentLoadLatencyCalibrationOperations& operations)
    -> std::expected<DependentLoadLatencyCalibrationOutcome, core::Error>;

} // namespace epycbench::measurement
