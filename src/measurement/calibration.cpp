#include "epycbench/measurement/calibration.hpp"
#include "measurement/prepared_execution_control.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::measurement {
namespace {

constexpr std::uint64_t latency_quantum = 4096U;

[[nodiscard]] auto invalid_duration_policy_error() -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{"measurement.calibration.invalid_duration_policy"},
        .message = "Measurement calibration duration policy is invalid",
        .operation = "measurement.calibration.duration_policy.create",
        .os_error = std::nullopt,
        .context = {{"classification", "duration_order_invalid"}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

} // namespace

auto detail::CalibrationAccess::make_outcome(
    CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
    TimerOverheadEstimate estimate,
    std::vector<SequentialReadBandwidthCalibrationTrial> candidate_trials,
    CalibrationDisposition disposition,
    std::optional<kernel::SequentialReadBandwidthPassCount> selected) noexcept
    -> SequentialReadBandwidthCalibrationOutcome {
    return {std::move(policy), std::move(empty_trials), estimate, std::move(candidate_trials),
            disposition, selected};
}

auto detail::CalibrationAccess::make_outcome(
    CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
    TimerOverheadEstimate estimate,
    std::vector<DependentLoadLatencyCalibrationTrial> candidate_trials,
    CalibrationDisposition disposition,
    std::optional<kernel::DependentLoadLatencyStepCount> selected) noexcept
    -> DependentLoadLatencyCalibrationOutcome {
    return {std::move(policy), std::move(empty_trials), estimate, std::move(candidate_trials),
            disposition, selected};
}

CalibrationDurationPolicy::CalibrationDurationPolicy(core::Duration minimum,
                                                     core::Duration target,
                                                     core::Duration maximum) noexcept
    : minimum_duration_{minimum}, target_duration_{target}, maximum_duration_{maximum} {}

auto CalibrationDurationPolicy::create(core::Duration minimum, core::Duration target,
                                       core::Duration maximum)
    -> std::expected<CalibrationDurationPolicy, core::Error> {
    if (minimum > target || target > maximum) {
        return std::unexpected{invalid_duration_policy_error()};
    }
    return CalibrationDurationPolicy{minimum, target, maximum};
}
auto CalibrationDurationPolicy::minimum_duration() const noexcept -> core::Duration {
    return minimum_duration_;
}
auto CalibrationDurationPolicy::target_duration() const noexcept -> core::Duration {
    return target_duration_;
}
auto CalibrationDurationPolicy::maximum_duration() const noexcept -> core::Duration {
    return maximum_duration_;
}

TimerOverheadEstimate::TimerOverheadEstimate(core::Duration duration) noexcept
    : duration_{duration} {}
auto TimerOverheadEstimate::create(core::Duration duration) noexcept -> TimerOverheadEstimate {
    return TimerOverheadEstimate{duration};
}
auto TimerOverheadEstimate::duration() const noexcept -> core::Duration { return duration_; }

SequentialReadBandwidthCalibrationTrial::SequentialReadBandwidthCalibrationTrial(
    kernel::SequentialReadBandwidthPassCount candidate, MeasurementInterval interval) noexcept
    : candidate_{candidate}, interval_{std::move(interval)} {}
auto SequentialReadBandwidthCalibrationTrial::create(
    kernel::SequentialReadBandwidthPassCount candidate, MeasurementInterval interval) noexcept
    -> SequentialReadBandwidthCalibrationTrial {
    return {candidate, std::move(interval)};
}
auto SequentialReadBandwidthCalibrationTrial::candidate() const noexcept
    -> kernel::SequentialReadBandwidthPassCount {
    return candidate_;
}
auto SequentialReadBandwidthCalibrationTrial::interval() const noexcept
    -> const MeasurementInterval& {
    return interval_;
}

DependentLoadLatencyCalibrationTrial::DependentLoadLatencyCalibrationTrial(
    kernel::DependentLoadLatencyStepCount candidate, MeasurementInterval interval) noexcept
    : candidate_{candidate}, interval_{std::move(interval)} {}
auto DependentLoadLatencyCalibrationTrial::create(
    kernel::DependentLoadLatencyStepCount candidate, MeasurementInterval interval) noexcept
    -> DependentLoadLatencyCalibrationTrial {
    return {candidate, std::move(interval)};
}
auto DependentLoadLatencyCalibrationTrial::candidate() const noexcept
    -> kernel::DependentLoadLatencyStepCount {
    return candidate_;
}
auto DependentLoadLatencyCalibrationTrial::interval() const noexcept
    -> const MeasurementInterval& {
    return interval_;
}

SequentialReadBandwidthCalibrationOutcome::SequentialReadBandwidthCalibrationOutcome(
    CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
    TimerOverheadEstimate estimate,
    std::vector<SequentialReadBandwidthCalibrationTrial> candidate_trials,
    CalibrationDisposition disposition,
    std::optional<kernel::SequentialReadBandwidthPassCount> selected) noexcept
    : policy_{std::move(policy)}, empty_trials_{std::move(empty_trials)}, estimate_{estimate},
      candidate_trials_{std::move(candidate_trials)}, disposition_{disposition},
      selected_{selected} {}
auto SequentialReadBandwidthCalibrationOutcome::duration_policy() const noexcept
    -> const CalibrationDurationPolicy& {
    return policy_;
}
auto SequentialReadBandwidthCalibrationOutcome::empty_harness_trials() const noexcept
    -> std::span<const EmptyHarnessTrial> {
    return empty_trials_;
}
auto SequentialReadBandwidthCalibrationOutcome::timer_overhead_estimate() const noexcept
    -> TimerOverheadEstimate {
    return estimate_;
}
auto SequentialReadBandwidthCalibrationOutcome::candidate_trials() const noexcept
    -> std::span<const SequentialReadBandwidthCalibrationTrial> {
    return candidate_trials_;
}
auto SequentialReadBandwidthCalibrationOutcome::disposition() const noexcept
    -> CalibrationDisposition {
    return disposition_;
}
auto SequentialReadBandwidthCalibrationOutcome::selected_candidate() const noexcept
    -> std::optional<kernel::SequentialReadBandwidthPassCount> {
    return selected_;
}

DependentLoadLatencyCalibrationOutcome::DependentLoadLatencyCalibrationOutcome(
    CalibrationDurationPolicy policy, std::vector<EmptyHarnessTrial> empty_trials,
    TimerOverheadEstimate estimate,
    std::vector<DependentLoadLatencyCalibrationTrial> candidate_trials,
    CalibrationDisposition disposition,
    std::optional<kernel::DependentLoadLatencyStepCount> selected) noexcept
    : policy_{std::move(policy)}, empty_trials_{std::move(empty_trials)}, estimate_{estimate},
      candidate_trials_{std::move(candidate_trials)}, disposition_{disposition},
      selected_{selected} {}
auto DependentLoadLatencyCalibrationOutcome::duration_policy() const noexcept
    -> const CalibrationDurationPolicy& {
    return policy_;
}
auto DependentLoadLatencyCalibrationOutcome::empty_harness_trials() const noexcept
    -> std::span<const EmptyHarnessTrial> {
    return empty_trials_;
}
auto DependentLoadLatencyCalibrationOutcome::timer_overhead_estimate() const noexcept
    -> TimerOverheadEstimate {
    return estimate_;
}
auto DependentLoadLatencyCalibrationOutcome::candidate_trials() const noexcept
    -> std::span<const DependentLoadLatencyCalibrationTrial> {
    return candidate_trials_;
}
auto DependentLoadLatencyCalibrationOutcome::disposition() const noexcept
    -> CalibrationDisposition {
    return disposition_;
}
auto DependentLoadLatencyCalibrationOutcome::selected_candidate() const noexcept
    -> std::optional<kernel::DependentLoadLatencyStepCount> {
    return selected_;
}

auto calibrate_sequential_read_bandwidth(
    CalibrationDurationPolicy policy, const detail::RawMonotonicClockOperations& clock,
    const detail::SequentialReadBandwidthCalibrationOperations& operations)
    -> std::expected<SequentialReadBandwidthCalibrationOutcome, core::Error> {
    std::vector<EmptyHarnessTrial> empty;
    std::optional<std::vector<SequentialReadBandwidthCalibrationTrial>> candidates;
    return detail::calibration_control<kernel::SequentialReadBandwidthPassCount,
                                       SequentialReadBandwidthCalibrationTrial,
                                       SequentialReadBandwidthCalibrationOutcome>(
        std::move(policy), empty,
        [&]() -> auto& { return candidates.emplace(); },
        [&] { empty.reserve(detail::prepared_empty_trial_count); },
        [&](auto& history) {
            history.reserve(detail::prepared_candidate_trial_bound);
        },
        [&] { return detail::EmptyHarnessAccess::run(clock); },
        [&](auto count) { return operations.run(operations.context, count); },
        std::numeric_limits<std::uint64_t>::max(),
        [](std::uint64_t units) {
            return *kernel::SequentialReadBandwidthPassCount::create(units);
        },
        [](auto count, MeasurementInterval interval) {
            return SequentialReadBandwidthCalibrationTrial::create(count,
                                                                    std::move(interval));
        },
        [](auto&&... arguments) {
            return detail::CalibrationAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
}

auto calibrate_dependent_load_latency(
    CalibrationDurationPolicy policy, const detail::RawMonotonicClockOperations& clock,
    const detail::DependentLoadLatencyCalibrationOperations& operations)
    -> std::expected<DependentLoadLatencyCalibrationOutcome, core::Error> {
    constexpr auto maximum_units = std::numeric_limits<std::uint64_t>::max() / latency_quantum;
    std::vector<EmptyHarnessTrial> empty;
    std::optional<std::vector<DependentLoadLatencyCalibrationTrial>> candidates;
    return detail::calibration_control<kernel::DependentLoadLatencyStepCount,
                                       DependentLoadLatencyCalibrationTrial,
                                       DependentLoadLatencyCalibrationOutcome>(
        std::move(policy), empty,
        [&]() -> auto& { return candidates.emplace(); },
        [&] { empty.reserve(detail::prepared_empty_trial_count); },
        [&](auto& history) {
            history.reserve(detail::prepared_candidate_trial_bound);
        },
        [&] { return detail::EmptyHarnessAccess::run(clock); },
        [&](auto count) { return operations.run(operations.context, count); },
        maximum_units,
        [](std::uint64_t units) {
            return *kernel::DependentLoadLatencyStepCount::create(units * latency_quantum);
        },
        [](auto count, MeasurementInterval interval) {
            return DependentLoadLatencyCalibrationTrial::create(count,
                                                                std::move(interval));
        },
        [](auto&&... arguments) {
            return detail::CalibrationAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
}

} // namespace epycbench::measurement
