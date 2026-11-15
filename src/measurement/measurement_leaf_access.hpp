#pragma once

#include "epycbench/measurement/calibration.hpp"
#include "epycbench/measurement/observation.hpp"
#include "epycbench/measurement/repetition.hpp"
#include "epycbench/measurement/validity.hpp"
#include "epycbench/measurement/warmup.hpp"

#include <algorithm>
#include <exception>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace epycbench::measurement::detail {

enum class RepetitionAccessError { invalid_transition, result_identity_mismatch };

struct MeasurementLeafAccess final {
    [[nodiscard]] static auto calibration_policy(core::Duration minimum,
                                                 core::Duration target,
                                                 core::Duration maximum) noexcept
        -> CalibrationDurationPolicy {
        return CalibrationDurationPolicy{minimum, target, maximum};
    }
    [[nodiscard]] static auto warmup_threshold(std::uint64_t numerator,
                                               std::uint64_t denominator) noexcept
        -> WarmupStabilityThreshold {
        return WarmupStabilityThreshold{numerator, denominator};
    }
    [[nodiscard]] static auto warmup_policy(
        std::uint64_t minimum, std::uint64_t maximum, std::uint64_t window,
        WarmupStabilityThreshold threshold) noexcept -> WarmupPolicy {
        return WarmupPolicy{minimum, maximum, window, threshold};
    }
    [[nodiscard]] static auto interval(platform_linux::RawMonotonicTimestamp start,
                                       platform_linux::RawMonotonicTimestamp end) noexcept
        -> MeasurementInterval {
        return MeasurementInterval{
            start, end,
            core::Duration::from_base_units(end.nanoseconds() - start.nanoseconds())};
    }
    [[nodiscard]] static auto maximum_overhead(std::uint64_t numerator,
                                               std::uint64_t denominator) noexcept
        -> MaximumHarnessOverhead {
        return MaximumHarnessOverhead{numerator, denominator};
    }
    [[nodiscard]] static auto repetition_policy(std::uint64_t minimum,
                                                std::uint64_t maximum,
                                                std::uint64_t minimum_valid,
                                                bool stopping) noexcept
        -> RepetitionPolicy {
        return RepetitionPolicy{minimum, maximum, minimum_valid, stopping};
    }
    [[nodiscard]] static auto repetition_outcome(
        core::ExperimentId experiment_id, RepetitionPolicy policy,
        RepetitionDisposition disposition, std::uint64_t completed_blocks,
        std::uint64_t complete_valid_blocks, std::uint64_t valid_first,
        std::uint64_t valid_second) noexcept -> RepetitionOutcome {
        return RepetitionOutcome{experiment_id, std::move(policy), disposition,
                                 completed_blocks, complete_valid_blocks,
                                 valid_first, valid_second};
    }
    [[nodiscard]] static auto bandwidth_observation(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::SequentialReadBandwidthConfig configuration,
        kernel::SequentialReadBandwidthPassCount pass_count,
        std::vector<SequentialReadBandwidthWorkerObservation> workers,
        MeasurementInterval interval, core::ByteCount aggregate) noexcept
        -> SequentialReadBandwidthObservation {
        return SequentialReadBandwidthObservation{
            observation_id, experiment_id, block_id, implementation_id,
            std::move(configuration), pass_count, std::move(workers), std::move(interval),
            aggregate};
    }
    [[nodiscard]] static auto latency_observation(
        core::ObservationId observation_id, core::ExperimentId experiment_id,
        kernel::DependentLoadLatencyBlockId block_id,
        kernel::KernelImplementationId implementation_id,
        kernel::DependentLoadLatencyConfig configuration,
        std::vector<DependentLoadLatencyWorkerObservation> workers,
        MeasurementInterval interval, core::WorkCount aggregate) noexcept
        -> DependentLoadLatencyObservation {
        return DependentLoadLatencyObservation{
            observation_id, experiment_id, block_id, implementation_id,
            std::move(configuration), std::move(workers), std::move(interval), aggregate};
    }

    template <class State, class Selected>
    [[nodiscard]] static auto initialize_repetition(
        core::ExperimentId experiment_id, RepetitionPolicy policy,
        ComparisonOrderPolicy comparison_order, bool calibration_enabled,
        WarmupPolicy warmup_policy, std::optional<Selected> selected) noexcept -> State {
        State state{experiment_id, std::move(policy), std::move(comparison_order),
                    calibration_enabled, std::move(warmup_policy), std::move(selected)};
        if (state.policy_.maximum_blocks() == 0U) {
            complete_repetition(state, RepetitionDisposition::maximum_blocks_reached);
        } else if (state.selected_state_) {
            after_repetition_selection(state);
        } else if (state.calibration_enabled_) {
            state.calibration_requested_ = true;
            state.phase_ = RepetitionPhase::calibration;
        } else {
            complete_repetition(state, RepetitionDisposition::selected_work_unavailable);
        }
        return state;
    }

    template <class State, class Selected>
    [[nodiscard]] static auto apply_calibration(
        State state, std::optional<Selected> selected) noexcept
        -> std::expected<State, RepetitionAccessError> {
        if (state.phase_ != RepetitionPhase::calibration)
            return std::unexpected{RepetitionAccessError::invalid_transition};
        if (!selected) {
            complete_repetition(state, RepetitionDisposition::selected_work_unavailable);
            return state;
        }
        state.selected_state_.emplace(std::move(*selected));
        after_repetition_selection(state);
        return state;
    }

    template <class State>
    [[nodiscard]] static auto apply_warmup(State state) noexcept
        -> std::expected<State, RepetitionAccessError> {
        if (state.phase_ != RepetitionPhase::warmup)
            return std::unexpected{RepetitionAccessError::invalid_transition};
        begin_repetition_block(state);
        return state;
    }

    template <class State, class Observation>
    [[nodiscard]] static auto apply_observation(
        State state, const Observation& observation,
        const ObservationValidityEvidence& validity) noexcept
        -> std::expected<State, RepetitionAccessError> {
        if (state.phase_ != RepetitionPhase::observation || !state.active_plan_)
            return std::unexpected{RepetitionAccessError::invalid_transition};
        const auto issued = core::ObservationId{state.next_observation_id_};
        if (observation.observation_id() != issued || validity.observation_id() != issued ||
            observation.block_id() != state.active_plan_->block_id())
            return std::unexpected{RepetitionAccessError::result_identity_mismatch};

        state.block_validity_[state.active_slot_] = validity.disposition();
        if (state.next_observation_id_ != std::numeric_limits<std::uint64_t>::max())
            ++state.next_observation_id_;
        ++state.active_slot_;
        if (state.active_slot_ < state.block_validity_.size())
            return state;

        ++state.completed_blocks_;
        const auto all_valid = std::ranges::all_of(
            state.block_validity_, [](auto disposition) {
                return disposition == ObservationValidityDisposition::valid;
            });
        if (all_valid) {
            ++state.complete_valid_blocks_;
            state.valid_first_variant_observations_ += 2U;
            state.valid_second_variant_observations_ += 2U;
        }
        state.active_plan_.reset();
        const auto eligible =
            state.policy_.stopping_enabled() && state.completed_blocks_ > 0U &&
            state.completed_blocks_ >= state.policy_.minimum_blocks() &&
            state.valid_first_variant_observations_ >=
                state.policy_.minimum_valid_observations_per_variant() &&
            state.valid_second_variant_observations_ >=
                state.policy_.minimum_valid_observations_per_variant();
        if (eligible) {
            state.phase_ = RepetitionPhase::precision;
        } else if (state.completed_blocks_ >= state.policy_.maximum_blocks()) {
            complete_repetition(state, RepetitionDisposition::maximum_blocks_reached);
        } else {
            begin_repetition_block(state);
        }
        return state;
    }

    template <class State>
    [[nodiscard]] static auto apply_precision(
        State state, PrecisionStoppingDecision decision) noexcept
        -> std::expected<State, RepetitionAccessError> {
        if (state.phase_ != RepetitionPhase::precision)
            return std::unexpected{RepetitionAccessError::invalid_transition};
        if (decision == PrecisionStoppingDecision::target_met) {
            complete_repetition(state, RepetitionDisposition::precision_target_reached);
        } else if (state.completed_blocks_ >= state.policy_.maximum_blocks()) {
            complete_repetition(state, RepetitionDisposition::maximum_blocks_reached);
        } else {
            begin_repetition_block(state);
        }
        return state;
    }

    [[nodiscard]] static auto next_action(
        const SequentialReadBandwidthRepetitionState& state) noexcept
        -> SequentialReadBandwidthRepetitionAction {
        switch (state.phase_) {
        case RepetitionPhase::calibration:
            return SequentialReadBandwidthCalibrationRequiredAction{};
        case RepetitionPhase::warmup:
            return SequentialReadBandwidthWarmupRequiredAction{*state.selected_state_};
        case RepetitionPhase::observation:
            return SequentialReadBandwidthObservationRequiredAction{
                state.experiment_id_, core::ObservationId{state.next_observation_id_},
                *state.active_plan_, state.active_slot_, *state.selected_state_};
        case RepetitionPhase::precision:
            return PrecisionEvaluationRequiredAction{state.completed_blocks_,
                                                     state.complete_valid_blocks_};
        case RepetitionPhase::complete:
            return RepetitionCompleteAction{*state.outcome_};
        }
        std::terminate();
    }

    [[nodiscard]] static auto next_action(
        const DependentLoadLatencyRepetitionState& state) noexcept
        -> DependentLoadLatencyRepetitionAction {
        switch (state.phase_) {
        case RepetitionPhase::calibration:
            return DependentLoadLatencyCalibrationRequiredAction{};
        case RepetitionPhase::warmup:
            return DependentLoadLatencyWarmupRequiredAction{*state.selected_state_};
        case RepetitionPhase::observation:
            return DependentLoadLatencyObservationRequiredAction{
                state.experiment_id_, core::ObservationId{state.next_observation_id_},
                *state.active_plan_, state.active_slot_, *state.selected_state_};
        case RepetitionPhase::precision:
            return PrecisionEvaluationRequiredAction{state.completed_blocks_,
                                                     state.complete_valid_blocks_};
        case RepetitionPhase::complete:
            return RepetitionCompleteAction{*state.outcome_};
        }
        std::terminate();
    }

  private:
    template <class State>
    static auto complete_repetition(State& state,
                                    RepetitionDisposition disposition) noexcept -> void {
        state.outcome_.emplace(RepetitionOutcome{
            state.experiment_id_, state.policy_, disposition, state.completed_blocks_,
            state.complete_valid_blocks_, state.valid_first_variant_observations_,
            state.valid_second_variant_observations_});
        state.phase_ = RepetitionPhase::complete;
    }

    template <class State>
    static auto begin_repetition_block(State& state) noexcept -> void {
        const auto first_block = state.completed_blocks_ == 0U;
        const auto preparation = BlockPreparationPlan::create(
            first_block && state.calibration_requested_,
            first_block && state.warmup_requested_);
        state.active_plan_.emplace(MeasurementBlockPlan::create(
            kernel::DependentLoadLatencyBlockId{state.next_block_id_},
            state.comparison_order_, preparation));
        ++state.next_block_id_;
        state.active_slot_ = 0U;
        std::ranges::fill(state.block_validity_, ObservationValidityDisposition::invalid);
        state.phase_ = RepetitionPhase::observation;
    }

    template <class State>
    static auto after_repetition_selection(State& state) noexcept -> void {
        if (state.warmup_policy_.maximum_trials() != 0U) {
            state.warmup_requested_ = true;
            state.phase_ = RepetitionPhase::warmup;
        } else {
            begin_repetition_block(state);
        }
    }
};

} // namespace epycbench::measurement::detail
