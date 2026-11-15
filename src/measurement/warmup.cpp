#include "epycbench/measurement/warmup.hpp"
#include "measurement/prepared_execution_control.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace epycbench::measurement {
namespace {

[[nodiscard]] auto invalid_policy_error() -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{"measurement.warmup.invalid_policy"},
        .message = "Measurement warm-up policy is invalid",
        .operation = "measurement.warmup.policy.create",
        .os_error = std::nullopt,
        .context = {{"classification", "policy_invariant_invalid"}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

template <typename Count, typename Outcome, typename Operations>
[[nodiscard]] auto run_warmup(WarmupPolicy policy, Count selected_work,
                              const Operations& operations) -> std::expected<Outcome, core::Error> {
    std::vector<WarmupTrial> trials;
    return detail::warmup_control<Count, Outcome>(
        std::move(policy), selected_work, trials,
        [&](Count count) { return operations.run(operations.context, count); },
        [](std::span<const WarmupTrial> values, std::uint64_t window,
           WarmupStabilityThreshold threshold) {
            std::vector<std::uint64_t> sorted;
            sorted.reserve(static_cast<std::size_t>(window));
            sorted.resize(static_cast<std::size_t>(window));
            return detail::evaluate_latest_window_control(values,
                                                          static_cast<std::size_t>(window),
                                                          threshold, sorted);
        },
        [](auto&&... arguments) {
            return detail::WarmupAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
}

} // namespace

WarmupStabilityThreshold::WarmupStabilityThreshold(std::uint64_t numerator,
                                                   std::uint64_t denominator) noexcept
    : numerator_{numerator}, denominator_{denominator} {}
auto WarmupStabilityThreshold::create(std::uint64_t numerator, std::uint64_t denominator)
    -> std::expected<WarmupStabilityThreshold, core::Error> {
    if (denominator == 0U) {
        return std::unexpected{invalid_policy_error()};
    }
    return WarmupStabilityThreshold{numerator, denominator};
}
auto WarmupStabilityThreshold::numerator() const noexcept -> std::uint64_t { return numerator_; }
auto WarmupStabilityThreshold::denominator() const noexcept -> std::uint64_t {
    return denominator_;
}

WarmupPolicy::WarmupPolicy(std::uint64_t minimum_trials, std::uint64_t maximum_trials,
                           std::uint64_t stability_window,
                           WarmupStabilityThreshold stability_threshold) noexcept
    : minimum_trials_{minimum_trials}, maximum_trials_{maximum_trials},
      stability_window_{stability_window}, stability_threshold_{stability_threshold} {}
auto WarmupPolicy::create(std::uint64_t minimum_trials, std::uint64_t maximum_trials,
                          std::uint64_t stability_window,
                          WarmupStabilityThreshold stability_threshold)
    -> std::expected<WarmupPolicy, core::Error> {
    const auto disabled_invalid = maximum_trials == 0U && minimum_trials != 0U;
    const auto enabled_invalid =
        maximum_trials > 0U && (stability_window == 0U || stability_window > maximum_trials);
    if (minimum_trials > maximum_trials || disabled_invalid || enabled_invalid) {
        return std::unexpected{invalid_policy_error()};
    }
    return WarmupPolicy{minimum_trials, maximum_trials, stability_window, stability_threshold};
}
auto WarmupPolicy::minimum_trials() const noexcept -> std::uint64_t { return minimum_trials_; }
auto WarmupPolicy::maximum_trials() const noexcept -> std::uint64_t { return maximum_trials_; }
auto WarmupPolicy::stability_window() const noexcept -> std::uint64_t { return stability_window_; }
auto WarmupPolicy::stability_threshold() const noexcept -> WarmupStabilityThreshold {
    return stability_threshold_;
}

WarmupTrial::WarmupTrial(MeasurementInterval interval) noexcept : interval_{std::move(interval)} {}
auto WarmupTrial::create(MeasurementInterval interval) noexcept -> WarmupTrial {
    return WarmupTrial{std::move(interval)};
}
auto WarmupTrial::interval() const noexcept -> const MeasurementInterval& { return interval_; }

WarmupStabilityEvaluation::WarmupStabilityEvaluation(core::Duration median_duration,
                                                     core::Duration maximum_absolute_deviation,
                                                     bool stable) noexcept
    : median_duration_{median_duration}, maximum_absolute_deviation_{maximum_absolute_deviation},
      stable_{stable} {}
auto WarmupStabilityEvaluation::create(core::Duration median_duration,
                                       core::Duration maximum_absolute_deviation,
                                       bool stable) noexcept -> WarmupStabilityEvaluation {
    return {median_duration, maximum_absolute_deviation, stable};
}
auto WarmupStabilityEvaluation::median_duration() const noexcept -> core::Duration {
    return median_duration_;
}
auto WarmupStabilityEvaluation::maximum_absolute_deviation() const noexcept -> core::Duration {
    return maximum_absolute_deviation_;
}
auto WarmupStabilityEvaluation::stable() const noexcept -> bool { return stable_; }

SequentialReadBandwidthWarmupOutcome::SequentialReadBandwidthWarmupOutcome(
    WarmupPolicy policy, kernel::SequentialReadBandwidthPassCount selected_work,
    std::vector<WarmupTrial> trials, WarmupDisposition disposition,
    std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept
    : policy_{std::move(policy)}, selected_work_{selected_work}, trials_{std::move(trials)},
      disposition_{disposition}, final_evaluation_{final_evaluation} {}
auto SequentialReadBandwidthWarmupOutcome::policy() const noexcept -> const WarmupPolicy& {
    return policy_;
}
auto SequentialReadBandwidthWarmupOutcome::selected_work() const noexcept
    -> kernel::SequentialReadBandwidthPassCount {
    return selected_work_;
}
auto SequentialReadBandwidthWarmupOutcome::trials() const noexcept -> std::span<const WarmupTrial> {
    return trials_;
}
auto SequentialReadBandwidthWarmupOutcome::disposition() const noexcept -> WarmupDisposition {
    return disposition_;
}
auto SequentialReadBandwidthWarmupOutcome::final_stability_evaluation() const noexcept
    -> std::optional<WarmupStabilityEvaluation> {
    return final_evaluation_;
}

DependentLoadLatencyWarmupOutcome::DependentLoadLatencyWarmupOutcome(
    WarmupPolicy policy, kernel::DependentLoadLatencyStepCount selected_work,
    std::vector<WarmupTrial> trials, WarmupDisposition disposition,
    std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept
    : policy_{std::move(policy)}, selected_work_{selected_work}, trials_{std::move(trials)},
      disposition_{disposition}, final_evaluation_{final_evaluation} {}
auto DependentLoadLatencyWarmupOutcome::policy() const noexcept -> const WarmupPolicy& {
    return policy_;
}
auto DependentLoadLatencyWarmupOutcome::selected_work() const noexcept
    -> kernel::DependentLoadLatencyStepCount {
    return selected_work_;
}
auto DependentLoadLatencyWarmupOutcome::trials() const noexcept -> std::span<const WarmupTrial> {
    return trials_;
}
auto DependentLoadLatencyWarmupOutcome::disposition() const noexcept -> WarmupDisposition {
    return disposition_;
}
auto DependentLoadLatencyWarmupOutcome::final_stability_evaluation() const noexcept
    -> std::optional<WarmupStabilityEvaluation> {
    return final_evaluation_;
}

auto detail::WarmupAccess::make_outcome(
    WarmupPolicy policy, kernel::SequentialReadBandwidthPassCount selected_work,
    std::vector<WarmupTrial> trials, WarmupDisposition disposition,
    std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept
    -> SequentialReadBandwidthWarmupOutcome {
    return {std::move(policy), selected_work, std::move(trials), disposition, final_evaluation};
}
auto detail::WarmupAccess::make_outcome(
    WarmupPolicy policy, kernel::DependentLoadLatencyStepCount selected_work,
    std::vector<WarmupTrial> trials, WarmupDisposition disposition,
    std::optional<WarmupStabilityEvaluation> final_evaluation) noexcept
    -> DependentLoadLatencyWarmupOutcome {
    return {std::move(policy), selected_work, std::move(trials), disposition, final_evaluation};
}

auto run_sequential_read_bandwidth_warmup(
    WarmupPolicy policy, kernel::SequentialReadBandwidthPassCount selected_work,
    const detail::SequentialReadBandwidthWarmupOperations& operations)
    -> std::expected<SequentialReadBandwidthWarmupOutcome, core::Error> {
    return run_warmup<kernel::SequentialReadBandwidthPassCount,
                      SequentialReadBandwidthWarmupOutcome>(std::move(policy), selected_work,
                                                            operations);
}
auto run_dependent_load_latency_warmup(
    WarmupPolicy policy, kernel::DependentLoadLatencyStepCount selected_work,
    const detail::DependentLoadLatencyWarmupOperations& operations)
    -> std::expected<DependentLoadLatencyWarmupOutcome, core::Error> {
    return run_warmup<kernel::DependentLoadLatencyStepCount, DependentLoadLatencyWarmupOutcome>(
        std::move(policy), selected_work, operations);
}

} // namespace epycbench::measurement
