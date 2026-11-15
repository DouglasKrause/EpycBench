#include "runtime/pairwise_measurement_leaf_facade.hpp"

#include "measurement/prepared_execution_control.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

thread_local void* allocation_context{};
thread_local void (*allocation_observer)(void*, std::size_t){};
thread_local std::array<std::size_t, 4> maximum_sizes{
    std::numeric_limits<std::size_t>::max(),
    std::numeric_limits<std::size_t>::max(),
    std::numeric_limits<std::size_t>::max(),
    std::numeric_limits<std::size_t>::max()};
thread_local std::array<std::uint64_t, 2> representation_limits{
    std::numeric_limits<std::uint64_t>::max(),
    std::numeric_limits<std::uint64_t>::max()};

[[nodiscard]] auto preparation_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return PairwiseEvidenceFailure{*value};
}

[[nodiscard]] auto same_policy(const measurement::CalibrationDurationPolicy& left,
                               const measurement::CalibrationDurationPolicy& right) noexcept
    -> bool {
    return left.minimum_duration() == right.minimum_duration() &&
           left.target_duration() == right.target_duration() &&
           left.maximum_duration() == right.maximum_duration();
}

[[nodiscard]] auto same_policy(const measurement::WarmupPolicy& left,
                               const measurement::WarmupPolicy& right) noexcept -> bool {
    const auto left_threshold = left.stability_threshold();
    const auto right_threshold = right.stability_threshold();
    return left.minimum_trials() == right.minimum_trials() &&
           left.maximum_trials() == right.maximum_trials() &&
           left.stability_window() == right.stability_window() &&
           left_threshold.numerator() == right_threshold.numerator() &&
           left_threshold.denominator() == right_threshold.denominator();
}

template <class Candidate>
struct PreparedStorage final {
    std::vector<measurement::EmptyHarnessTrial> empty;
    std::vector<Candidate> candidates;
    std::vector<measurement::WarmupTrial> warmup;
    std::vector<std::uint64_t> scratch;
};

template <class Candidate>
[[nodiscard]] auto prepare_storage(const measurement::WarmupPolicy& warmup)
    -> std::expected<PreparedStorage<Candidate>, PairwiseEvidenceFailure> {
    PreparedStorage<Candidate> result;
    const auto enabled = warmup.maximum_trials() != 0U;
    std::size_t history_size{};
    std::size_t scratch_size{};
    if (measurement::detail::prepared_empty_trial_count >
            std::min(maximum_sizes[0], result.empty.max_size()) ||
        measurement::detail::prepared_candidate_trial_bound >
            std::min(maximum_sizes[1], result.candidates.max_size()) ||
        (enabled &&
         (warmup.maximum_trials() > representation_limits[0] ||
          warmup.stability_window() > representation_limits[1] ||
          !std::in_range<std::size_t>(warmup.maximum_trials()) ||
          !std::in_range<std::size_t>(warmup.stability_window()))))
        return std::unexpected{
            preparation_failure(PairwiseFailureReason::contained_exception)};
    if (enabled) {
        history_size = static_cast<std::size_t>(warmup.maximum_trials());
        scratch_size = static_cast<std::size_t>(warmup.stability_window());
        if (history_size > std::min(maximum_sizes[2], result.warmup.max_size()) ||
            scratch_size > std::min(maximum_sizes[3], result.scratch.max_size()))
            return std::unexpected{
                preparation_failure(PairwiseFailureReason::contained_exception)};
    }
    try {
        std::size_t ordinal{};
        if (allocation_observer)
            allocation_observer(allocation_context, ordinal);
        ++ordinal;
        result.empty.reserve(measurement::detail::prepared_empty_trial_count);
        if (allocation_observer)
            allocation_observer(allocation_context, ordinal);
        ++ordinal;
        result.candidates.reserve(measurement::detail::prepared_candidate_trial_bound);
        if (enabled) {
            if (allocation_observer)
                allocation_observer(allocation_context, ordinal);
            ++ordinal;
            result.warmup.reserve(history_size);
            if (allocation_observer)
                allocation_observer(allocation_context, ordinal);
            result.scratch.resize(scratch_size);
        }
        return result;
    } catch (const std::exception&) {
        return std::unexpected{
            preparation_failure(PairwiseFailureReason::contained_exception)};
    } catch (...) {
        return std::unexpected{
            preparation_failure(PairwiseFailureReason::contained_unknown_failure)};
    }
}

template <class SourceResult>
void require_source_failure(const SourceResult& result) noexcept {
    if (!result && result.error().active_block_id)
        std::terminate();
}

} // namespace

void PairwisePreparedMeasurementExecutionTestAccess::install_allocation_observer(
    void* context, void (*observer)(void*, std::size_t)) noexcept {
    allocation_context = context;
    allocation_observer = observer;
}

void PairwisePreparedMeasurementExecutionTestAccess::install_max_sizes(
    std::size_t empty, std::size_t candidates, std::size_t warmup,
    std::size_t scratch) noexcept {
    maximum_sizes = {empty, candidates, warmup, scratch};
}

void PairwisePreparedMeasurementExecutionTestAccess::install_representation_limits(
    std::uint64_t warmup, std::uint64_t scratch) noexcept {
    representation_limits = {warmup, scratch};
}

void PairwisePreparedMeasurementExecutionTestAccess::clear_hooks() noexcept {
    allocation_context = nullptr;
    allocation_observer = nullptr;
    maximum_sizes.fill(std::numeric_limits<std::size_t>::max());
    representation_limits.fill(std::numeric_limits<std::uint64_t>::max());
}

PairwisePreparedBandwidthMeasurementExecution::
    PairwisePreparedBandwidthMeasurementExecution(
        measurement::CalibrationDurationPolicy calibration,
        measurement::WarmupPolicy warmup,
        std::vector<measurement::EmptyHarnessTrial>&& empty,
        std::vector<measurement::SequentialReadBandwidthCalibrationTrial>&& candidates,
        std::vector<measurement::WarmupTrial>&& warmup_trials,
        std::vector<std::uint64_t>&& scratch) noexcept
    : calibration_policy_{std::move(calibration)}, warmup_policy_{std::move(warmup)},
      empty_trials_{std::move(empty)}, candidate_trials_{std::move(candidates)},
      warmup_trials_{std::move(warmup_trials)}, warmup_scratch_{std::move(scratch)} {}

PairwisePreparedBandwidthMeasurementExecution::
    PairwisePreparedBandwidthMeasurementExecution(
        PairwisePreparedBandwidthMeasurementExecution&& other) noexcept
    : calibration_policy_{std::move(other.calibration_policy_)},
      warmup_policy_{std::move(other.warmup_policy_)},
      empty_trials_{std::move(other.empty_trials_)},
      candidate_trials_{std::move(other.candidate_trials_)},
      warmup_trials_{std::move(other.warmup_trials_)},
      warmup_scratch_{std::move(other.warmup_scratch_)},
      calibration_state_{other.calibration_state_}, warmup_state_{other.warmup_state_},
      moved_from_{other.moved_from_} {
    other.moved_from_ = true;
}

void PairwisePreparedBandwidthMeasurementExecution::require_calibration(
    const measurement::CalibrationDurationPolicy& policy) const noexcept {
    if (moved_from_ ||
        calibration_state_ != PairwisePreparedMeasurementComponentState::prepared ||
        !same_policy(calibration_policy_, policy))
        std::terminate();
}
void PairwisePreparedBandwidthMeasurementExecution::require_warmup(
    const measurement::WarmupPolicy& policy) const noexcept {
    if (moved_from_ ||
        warmup_state_ != PairwisePreparedMeasurementComponentState::prepared ||
        !same_policy(warmup_policy_, policy))
        std::terminate();
}

PairwisePreparedLatencyMeasurementExecution::PairwisePreparedLatencyMeasurementExecution(
    measurement::CalibrationDurationPolicy calibration, measurement::WarmupPolicy warmup,
    std::vector<measurement::EmptyHarnessTrial>&& empty,
    std::vector<measurement::DependentLoadLatencyCalibrationTrial>&& candidates,
    std::vector<measurement::WarmupTrial>&& warmup_trials,
    std::vector<std::uint64_t>&& scratch) noexcept
    : calibration_policy_{std::move(calibration)}, warmup_policy_{std::move(warmup)},
      empty_trials_{std::move(empty)}, candidate_trials_{std::move(candidates)},
      warmup_trials_{std::move(warmup_trials)}, warmup_scratch_{std::move(scratch)} {}

PairwisePreparedLatencyMeasurementExecution::PairwisePreparedLatencyMeasurementExecution(
    PairwisePreparedLatencyMeasurementExecution&& other) noexcept
    : calibration_policy_{std::move(other.calibration_policy_)},
      warmup_policy_{std::move(other.warmup_policy_)},
      empty_trials_{std::move(other.empty_trials_)},
      candidate_trials_{std::move(other.candidate_trials_)},
      warmup_trials_{std::move(other.warmup_trials_)},
      warmup_scratch_{std::move(other.warmup_scratch_)},
      calibration_state_{other.calibration_state_}, warmup_state_{other.warmup_state_},
      moved_from_{other.moved_from_} {
    other.moved_from_ = true;
}

void PairwisePreparedLatencyMeasurementExecution::require_calibration(
    const measurement::CalibrationDurationPolicy& policy) const noexcept {
    if (moved_from_ ||
        calibration_state_ != PairwisePreparedMeasurementComponentState::prepared ||
        !same_policy(calibration_policy_, policy))
        std::terminate();
}
void PairwisePreparedLatencyMeasurementExecution::require_warmup(
    const measurement::WarmupPolicy& policy) const noexcept {
    if (moved_from_ ||
        warmup_state_ != PairwisePreparedMeasurementComponentState::prepared ||
        !same_policy(warmup_policy_, policy))
        std::terminate();
}

auto PairwiseMeasurementLeafFacade::prepare_bandwidth_execution(
    const measurement::CalibrationDurationPolicy& calibration,
    const measurement::WarmupPolicy& warmup) const noexcept
    -> std::expected<PairwisePreparedBandwidthMeasurementExecution,
                     PairwiseEvidenceFailure> {
    auto storage = prepare_storage<measurement::SequentialReadBandwidthCalibrationTrial>(warmup);
    if (!storage)
        return std::unexpected{std::move(storage.error())};
    return PairwisePreparedBandwidthMeasurementExecution{
        calibration, warmup, std::move(storage->empty), std::move(storage->candidates),
        std::move(storage->warmup), std::move(storage->scratch)};
}

auto PairwiseMeasurementLeafFacade::prepare_latency_execution(
    const measurement::CalibrationDurationPolicy& calibration,
    const measurement::WarmupPolicy& warmup) const noexcept
    -> std::expected<PairwisePreparedLatencyMeasurementExecution,
                     PairwiseEvidenceFailure> {
    auto storage = prepare_storage<measurement::DependentLoadLatencyCalibrationTrial>(warmup);
    if (!storage)
        return std::unexpected{std::move(storage.error())};
    return PairwisePreparedLatencyMeasurementExecution{
        calibration, warmup, std::move(storage->empty), std::move(storage->candidates),
        std::move(storage->warmup), std::move(storage->scratch)};
}

auto PairwiseMeasurementLeafFacade::calibrate(
    PairwisePreparedBandwidthMeasurementExecution& prepared,
    const measurement::CalibrationDurationPolicy& policy,
    PairwiseTypedBandwidthCalibrationOperations operations) const noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::SequentialReadBandwidthCalibrationOutcome> {
    prepared.require_calibration(policy);
    if (!operations.run_empty || !operations.run_candidate ||
        prepared.empty_trials_.capacity() < measurement::detail::prepared_empty_trial_count ||
        prepared.candidate_trials_.capacity() <
            measurement::detail::prepared_candidate_trial_bound)
        std::terminate();
    prepared.calibration_state_ = PairwisePreparedMeasurementComponentState::consuming;
    auto result = measurement::detail::calibration_control<
        kernel::SequentialReadBandwidthPassCount,
        measurement::SequentialReadBandwidthCalibrationTrial,
        measurement::SequentialReadBandwidthCalibrationOutcome>(
        prepared.calibration_policy_, prepared.empty_trials_,
        [&]() -> auto& { return prepared.candidate_trials_; }, [] {}, [](auto&) {},
        [&] {
            auto value = operations.run_empty(operations.context);
            require_source_failure(value);
            return value;
        },
        [&](auto count) {
            auto value = operations.run_candidate(operations.context, count);
            require_source_failure(value);
            return value;
        },
        std::numeric_limits<std::uint64_t>::max(),
        [](std::uint64_t units) {
            return *kernel::SequentialReadBandwidthPassCount::create(units);
        },
        [](auto count, measurement::MeasurementInterval interval) {
            return measurement::SequentialReadBandwidthCalibrationTrial::create(
                count, std::move(interval));
        },
        [](auto&&... arguments) {
            return measurement::detail::CalibrationAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
    prepared.calibration_state_ = result
        ? PairwisePreparedMeasurementComponentState::succeeded
        : PairwisePreparedMeasurementComponentState::failed;
    return result;
}

auto PairwiseMeasurementLeafFacade::calibrate(
    PairwisePreparedLatencyMeasurementExecution& prepared,
    const measurement::CalibrationDurationPolicy& policy,
    PairwiseTypedLatencyCalibrationOperations operations) const noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::DependentLoadLatencyCalibrationOutcome> {
    prepared.require_calibration(policy);
    if (!operations.run_empty || !operations.run_candidate ||
        prepared.empty_trials_.capacity() < measurement::detail::prepared_empty_trial_count ||
        prepared.candidate_trials_.capacity() <
            measurement::detail::prepared_candidate_trial_bound)
        std::terminate();
    prepared.calibration_state_ = PairwisePreparedMeasurementComponentState::consuming;
    constexpr auto maximum_units = std::numeric_limits<std::uint64_t>::max() / 4096U;
    auto result = measurement::detail::calibration_control<
        kernel::DependentLoadLatencyStepCount,
        measurement::DependentLoadLatencyCalibrationTrial,
        measurement::DependentLoadLatencyCalibrationOutcome>(
        prepared.calibration_policy_, prepared.empty_trials_,
        [&]() -> auto& { return prepared.candidate_trials_; }, [] {}, [](auto&) {},
        [&] {
            auto value = operations.run_empty(operations.context);
            require_source_failure(value);
            return value;
        },
        [&](auto count) {
            auto value = operations.run_candidate(operations.context, count);
            require_source_failure(value);
            return value;
        },
        maximum_units,
        [](std::uint64_t units) {
            return *kernel::DependentLoadLatencyStepCount::create(units * 4096U);
        },
        [](auto count, measurement::MeasurementInterval interval) {
            return measurement::DependentLoadLatencyCalibrationTrial::create(
                count, std::move(interval));
        },
        [](auto&&... arguments) {
            return measurement::detail::CalibrationAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
    prepared.calibration_state_ = result
        ? PairwisePreparedMeasurementComponentState::succeeded
        : PairwisePreparedMeasurementComponentState::failed;
    return result;
}

auto PairwiseMeasurementLeafFacade::warmup(
    PairwisePreparedBandwidthMeasurementExecution& prepared,
    const measurement::WarmupPolicy& policy,
    kernel::SequentialReadBandwidthPassCount selected,
    PairwiseTypedBandwidthWarmupOperations operations) const noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::SequentialReadBandwidthWarmupOutcome> {
    prepared.require_warmup(policy);
    if (!operations.run ||
        prepared.warmup_trials_.capacity() < policy.maximum_trials() ||
        prepared.warmup_scratch_.size() !=
            (policy.maximum_trials() == 0U ? 0U : policy.stability_window()))
        std::terminate();
    prepared.warmup_state_ = PairwisePreparedMeasurementComponentState::consuming;
    auto result = measurement::detail::warmup_control<
        kernel::SequentialReadBandwidthPassCount,
        measurement::SequentialReadBandwidthWarmupOutcome>(
        prepared.warmup_policy_, selected, prepared.warmup_trials_,
        [&](auto count) {
            auto value = operations.run(operations.context, count);
            require_source_failure(value);
            return value;
        },
        [&](std::span<const measurement::WarmupTrial> trials, std::uint64_t window,
            measurement::WarmupStabilityThreshold threshold) {
            return measurement::detail::evaluate_latest_window_control(
                trials, static_cast<std::size_t>(window), threshold,
                prepared.warmup_scratch_);
        },
        [](auto&&... arguments) {
            return measurement::detail::WarmupAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
    prepared.warmup_state_ = result
        ? PairwisePreparedMeasurementComponentState::succeeded
        : PairwisePreparedMeasurementComponentState::failed;
    return result;
}

auto PairwiseMeasurementLeafFacade::warmup(
    PairwisePreparedLatencyMeasurementExecution& prepared,
    const measurement::WarmupPolicy& policy,
    kernel::DependentLoadLatencyStepCount selected,
    PairwiseTypedLatencyWarmupOperations operations) const noexcept
    -> PairwiseTypedOperationSourceResult<
        measurement::DependentLoadLatencyWarmupOutcome> {
    prepared.require_warmup(policy);
    if (!operations.run ||
        prepared.warmup_trials_.capacity() < policy.maximum_trials() ||
        prepared.warmup_scratch_.size() !=
            (policy.maximum_trials() == 0U ? 0U : policy.stability_window()))
        std::terminate();
    prepared.warmup_state_ = PairwisePreparedMeasurementComponentState::consuming;
    auto result = measurement::detail::warmup_control<
        kernel::DependentLoadLatencyStepCount,
        measurement::DependentLoadLatencyWarmupOutcome>(
        prepared.warmup_policy_, selected, prepared.warmup_trials_,
        [&](auto count) {
            auto value = operations.run(operations.context, count);
            require_source_failure(value);
            return value;
        },
        [&](std::span<const measurement::WarmupTrial> trials, std::uint64_t window,
            measurement::WarmupStabilityThreshold threshold) {
            return measurement::detail::evaluate_latest_window_control(
                trials, static_cast<std::size_t>(window), threshold,
                prepared.warmup_scratch_);
        },
        [](auto&&... arguments) {
            return measurement::detail::WarmupAccess::make_outcome(
                std::forward<decltype(arguments)>(arguments)...);
        });
    prepared.warmup_state_ = result
        ? PairwisePreparedMeasurementComponentState::succeeded
        : PairwisePreparedMeasurementComponentState::failed;
    return result;
}

} // namespace epycbench::runtime::detail
