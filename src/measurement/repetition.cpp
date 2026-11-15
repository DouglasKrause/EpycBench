#include "epycbench/measurement/repetition.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace epycbench::measurement {
namespace {

[[nodiscard]] auto repetition_error(std::string code, std::string operation,
                                    std::string classification) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{std::move(code)},
        .message = "Measurement repetition progression failed",
        .operation = std::move(operation),
        .os_error = std::nullopt,
        .context = {{"classification", std::move(classification)}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

[[nodiscard]] auto invalid_policy_error() -> core::Error {
    return repetition_error("measurement.repetition.invalid_policy",
                            "measurement.repetition.policy.create", "invalid_policy");
}

[[nodiscard]] auto invalid_transition_error() -> core::Error {
    return repetition_error("measurement.repetition.invalid_transition",
                            "measurement.repetition.transition.apply", "invalid_transition");
}

[[nodiscard]] auto result_identity_mismatch_error() -> core::Error {
    return repetition_error("measurement.repetition.result_identity_mismatch",
                            "measurement.repetition.observation.apply", "result_identity_mismatch");
}

} // namespace

RepetitionPolicy::RepetitionPolicy(std::uint64_t minimum_blocks, std::uint64_t maximum_blocks,
                                   std::uint64_t minimum_valid_observations_per_variant,
                                   bool stopping_enabled) noexcept
    : minimum_blocks_{minimum_blocks}, maximum_blocks_{maximum_blocks},
      minimum_valid_observations_per_variant_{minimum_valid_observations_per_variant},
      stopping_enabled_{stopping_enabled} {}

auto RepetitionPolicy::create(std::uint64_t minimum_blocks, std::uint64_t maximum_blocks,
                              std::uint64_t minimum_valid_observations_per_variant,
                              bool stopping_enabled)
    -> std::expected<RepetitionPolicy, core::Error> {
    constexpr auto observations_per_block = std::uint64_t{4U};
    const auto largest_safe_maximum =
        std::numeric_limits<std::uint64_t>::max() / observations_per_block + 1U;
    if (minimum_blocks > maximum_blocks || maximum_blocks > largest_safe_maximum) {
        return std::unexpected{invalid_policy_error()};
    }
    return RepetitionPolicy{minimum_blocks, maximum_blocks, minimum_valid_observations_per_variant,
                            stopping_enabled};
}

auto RepetitionPolicy::minimum_blocks() const noexcept -> std::uint64_t { return minimum_blocks_; }
auto RepetitionPolicy::maximum_blocks() const noexcept -> std::uint64_t { return maximum_blocks_; }
auto RepetitionPolicy::minimum_valid_observations_per_variant() const noexcept -> std::uint64_t {
    return minimum_valid_observations_per_variant_;
}
auto RepetitionPolicy::stopping_enabled() const noexcept -> bool { return stopping_enabled_; }

BandwidthSelectedExecutionState::BandwidthSelectedExecutionState(
    kernel::SequentialReadBandwidthPassCount selected_work,
    TimerOverheadEstimate timer_overhead_estimate) noexcept
    : selected_work_{selected_work}, timer_overhead_estimate_{timer_overhead_estimate} {}

auto BandwidthSelectedExecutionState::create(kernel::SequentialReadBandwidthPassCount selected_work,
                                             TimerOverheadEstimate timer_overhead_estimate) noexcept
    -> BandwidthSelectedExecutionState {
    return {selected_work, timer_overhead_estimate};
}
auto BandwidthSelectedExecutionState::selected_work() const noexcept
    -> kernel::SequentialReadBandwidthPassCount {
    return selected_work_;
}
auto BandwidthSelectedExecutionState::timer_overhead_estimate() const noexcept
    -> TimerOverheadEstimate {
    return timer_overhead_estimate_;
}

LatencySelectedExecutionState::LatencySelectedExecutionState(
    kernel::DependentLoadLatencyStepCount selected_work,
    TimerOverheadEstimate timer_overhead_estimate) noexcept
    : selected_work_{selected_work}, timer_overhead_estimate_{timer_overhead_estimate} {}

auto LatencySelectedExecutionState::create(kernel::DependentLoadLatencyStepCount selected_work,
                                           TimerOverheadEstimate timer_overhead_estimate) noexcept
    -> LatencySelectedExecutionState {
    return {selected_work, timer_overhead_estimate};
}
auto LatencySelectedExecutionState::selected_work() const noexcept
    -> kernel::DependentLoadLatencyStepCount {
    return selected_work_;
}
auto LatencySelectedExecutionState::timer_overhead_estimate() const noexcept
    -> TimerOverheadEstimate {
    return timer_overhead_estimate_;
}

RepetitionOutcome::RepetitionOutcome(core::ExperimentId experiment_id, RepetitionPolicy policy,
                                     RepetitionDisposition disposition,
                                     std::uint64_t completed_blocks,
                                     std::uint64_t complete_valid_blocks,
                                     std::uint64_t valid_first_variant_observations,
                                     std::uint64_t valid_second_variant_observations) noexcept
    : experiment_id_{experiment_id}, policy_{std::move(policy)}, disposition_{disposition},
      completed_blocks_{completed_blocks}, complete_valid_blocks_{complete_valid_blocks},
      valid_first_variant_observations_{valid_first_variant_observations},
      valid_second_variant_observations_{valid_second_variant_observations} {}
auto RepetitionOutcome::experiment_id() const noexcept -> core::ExperimentId {
    return experiment_id_;
}
auto RepetitionOutcome::policy() const noexcept -> const RepetitionPolicy& { return policy_; }
auto RepetitionOutcome::disposition() const noexcept -> RepetitionDisposition {
    return disposition_;
}
auto RepetitionOutcome::completed_blocks() const noexcept -> std::uint64_t {
    return completed_blocks_;
}
auto RepetitionOutcome::complete_valid_blocks() const noexcept -> std::uint64_t {
    return complete_valid_blocks_;
}
auto RepetitionOutcome::valid_first_variant_observations() const noexcept -> std::uint64_t {
    return valid_first_variant_observations_;
}
auto RepetitionOutcome::valid_second_variant_observations() const noexcept -> std::uint64_t {
    return valid_second_variant_observations_;
}

SequentialReadBandwidthWarmupRequiredAction::SequentialReadBandwidthWarmupRequiredAction(
    BandwidthSelectedExecutionState selected_state) noexcept
    : selected_state_{std::move(selected_state)} {}
auto SequentialReadBandwidthWarmupRequiredAction::selected_state() const noexcept
    -> const BandwidthSelectedExecutionState& {
    return selected_state_;
}
DependentLoadLatencyWarmupRequiredAction::DependentLoadLatencyWarmupRequiredAction(
    LatencySelectedExecutionState selected_state) noexcept
    : selected_state_{std::move(selected_state)} {}
auto DependentLoadLatencyWarmupRequiredAction::selected_state() const noexcept
    -> const LatencySelectedExecutionState& {
    return selected_state_;
}

SequentialReadBandwidthObservationRequiredAction::SequentialReadBandwidthObservationRequiredAction(
    core::ExperimentId experiment_id, core::ObservationId observation_id,
    MeasurementBlockPlan block_plan, std::uint8_t slot_index,
    BandwidthSelectedExecutionState selected_state) noexcept
    : experiment_id_{experiment_id}, observation_id_{observation_id},
      block_plan_{std::move(block_plan)}, slot_index_{slot_index},
      selected_state_{std::move(selected_state)} {}
auto SequentialReadBandwidthObservationRequiredAction::experiment_id() const noexcept
    -> core::ExperimentId {
    return experiment_id_;
}
auto SequentialReadBandwidthObservationRequiredAction::observation_id() const noexcept
    -> core::ObservationId {
    return observation_id_;
}
auto SequentialReadBandwidthObservationRequiredAction::block_plan() const noexcept
    -> const MeasurementBlockPlan& {
    return block_plan_;
}
auto SequentialReadBandwidthObservationRequiredAction::slot() const noexcept
    -> const ScheduledObservationSlot& {
    return block_plan_.observations()[slot_index_];
}
auto SequentialReadBandwidthObservationRequiredAction::selected_state() const noexcept
    -> const BandwidthSelectedExecutionState& {
    return selected_state_;
}

DependentLoadLatencyObservationRequiredAction::DependentLoadLatencyObservationRequiredAction(
    core::ExperimentId experiment_id, core::ObservationId observation_id,
    MeasurementBlockPlan block_plan, std::uint8_t slot_index,
    LatencySelectedExecutionState selected_state) noexcept
    : experiment_id_{experiment_id}, observation_id_{observation_id},
      block_plan_{std::move(block_plan)}, slot_index_{slot_index},
      selected_state_{std::move(selected_state)} {}
auto DependentLoadLatencyObservationRequiredAction::experiment_id() const noexcept
    -> core::ExperimentId {
    return experiment_id_;
}
auto DependentLoadLatencyObservationRequiredAction::observation_id() const noexcept
    -> core::ObservationId {
    return observation_id_;
}
auto DependentLoadLatencyObservationRequiredAction::block_plan() const noexcept
    -> const MeasurementBlockPlan& {
    return block_plan_;
}
auto DependentLoadLatencyObservationRequiredAction::slot() const noexcept
    -> const ScheduledObservationSlot& {
    return block_plan_.observations()[slot_index_];
}
auto DependentLoadLatencyObservationRequiredAction::selected_state() const noexcept
    -> const LatencySelectedExecutionState& {
    return selected_state_;
}

PrecisionEvaluationRequiredAction::PrecisionEvaluationRequiredAction(
    std::uint64_t completed_blocks, std::uint64_t complete_valid_blocks) noexcept
    : completed_blocks_{completed_blocks}, complete_valid_blocks_{complete_valid_blocks} {}
auto PrecisionEvaluationRequiredAction::completed_blocks() const noexcept -> std::uint64_t {
    return completed_blocks_;
}
auto PrecisionEvaluationRequiredAction::complete_valid_blocks() const noexcept -> std::uint64_t {
    return complete_valid_blocks_;
}
RepetitionCompleteAction::RepetitionCompleteAction(RepetitionOutcome outcome) noexcept
    : outcome_{std::move(outcome)} {}
auto RepetitionCompleteAction::outcome() const noexcept -> const RepetitionOutcome& {
    return outcome_;
}

class detail_RepetitionAccess {
  public:
    template <typename State>
    static auto complete(State& state, RepetitionDisposition disposition) -> void {
        state.outcome_.emplace(RepetitionOutcome{
            state.experiment_id_, state.policy_, disposition, state.completed_blocks_,
            state.complete_valid_blocks_, state.valid_first_variant_observations_,
            state.valid_second_variant_observations_});
        state.phase_ = RepetitionPhase::complete;
    }

    template <typename State> static auto begin_block(State& state) -> void {
        const auto first_block = state.completed_blocks_ == 0U;
        const auto preparation = BlockPreparationPlan::create(
            first_block && state.calibration_requested_, first_block && state.warmup_requested_);
        state.active_plan_.emplace(
            MeasurementBlockPlan::create(kernel::DependentLoadLatencyBlockId{state.next_block_id_},
                                         state.comparison_order_, preparation));
        ++state.next_block_id_;
        state.active_slot_ = 0U;
        std::ranges::fill(state.block_validity_, ObservationValidityDisposition::invalid);
        state.phase_ = RepetitionPhase::observation;
    }

    template <typename State> static auto after_selection(State& state) -> void {
        if (state.warmup_policy_.maximum_trials() != 0U) {
            state.warmup_requested_ = true;
            state.phase_ = RepetitionPhase::warmup;
        } else {
            begin_block(state);
        }
    }

    template <typename State, typename Selected>
    static auto initialize(core::ExperimentId experiment_id, RepetitionPolicy policy,
                           ComparisonOrderPolicy comparison_order, bool calibration_enabled,
                           WarmupPolicy warmup_policy, std::optional<Selected> selected_state)
        -> State {
        State state{experiment_id,       std::move(policy),        std::move(comparison_order),
                    calibration_enabled, std::move(warmup_policy), std::move(selected_state)};
        if (state.policy_.maximum_blocks() == 0U) {
            complete(state, RepetitionDisposition::maximum_blocks_reached);
        } else if (state.selected_state_) {
            after_selection(state);
        } else if (state.calibration_enabled_) {
            state.calibration_requested_ = true;
            state.phase_ = RepetitionPhase::calibration;
        } else {
            complete(state, RepetitionDisposition::selected_work_unavailable);
        }
        return state;
    }

    template <typename State, typename Selected>
    static auto apply_calibration(State state, RepetitionPhase expected,
                                  std::optional<Selected> selected)
        -> std::expected<State, core::Error> {
        if (state.phase_ != expected)
            return std::unexpected{invalid_transition_error()};
        if (!selected) {
            complete(state, RepetitionDisposition::selected_work_unavailable);
            return state;
        }
        state.selected_state_.emplace(std::move(*selected));
        after_selection(state);
        return state;
    }

    template <typename State>
    static auto apply_warmup(State state) -> std::expected<State, core::Error> {
        if (state.phase_ != RepetitionPhase::warmup)
            return std::unexpected{invalid_transition_error()};
        begin_block(state);
        return state;
    }

    template <typename State, typename Observation>
    static auto apply_observation(State state, const Observation& observation,
                                  const ObservationValidityEvidence& validity)
        -> std::expected<State, core::Error> {
        if (state.phase_ != RepetitionPhase::observation || !state.active_plan_)
            return std::unexpected{invalid_transition_error()};
        const auto issued = core::ObservationId{state.next_observation_id_};
        if (observation.observation_id() != issued || validity.observation_id() != issued ||
            observation.block_id() != state.active_plan_->block_id()) {
            return std::unexpected{result_identity_mismatch_error()};
        }
        state.block_validity_[state.active_slot_] = validity.disposition();
        if (state.next_observation_id_ != std::numeric_limits<std::uint64_t>::max()) {
            ++state.next_observation_id_;
        }
        ++state.active_slot_;
        if (state.active_slot_ < 4U)
            return state;

        ++state.completed_blocks_;
        const auto all_valid = std::ranges::all_of(state.block_validity_, [](auto disposition) {
            return disposition == ObservationValidityDisposition::valid;
        });
        if (all_valid) {
            ++state.complete_valid_blocks_;
            state.valid_first_variant_observations_ += 2U;
            state.valid_second_variant_observations_ += 2U;
        }
        state.active_plan_.reset();
        const auto eligible = state.policy_.stopping_enabled() && state.completed_blocks_ > 0U &&
                              state.completed_blocks_ >= state.policy_.minimum_blocks() &&
                              state.valid_first_variant_observations_ >=
                                  state.policy_.minimum_valid_observations_per_variant() &&
                              state.valid_second_variant_observations_ >=
                                  state.policy_.minimum_valid_observations_per_variant();
        if (eligible) {
            state.phase_ = RepetitionPhase::precision;
        } else if (state.completed_blocks_ >= state.policy_.maximum_blocks()) {
            complete(state, RepetitionDisposition::maximum_blocks_reached);
        } else {
            begin_block(state);
        }
        return state;
    }

    template <typename State>
    static auto apply_precision(State state, PrecisionStoppingDecision decision)
        -> std::expected<State, core::Error> {
        if (state.phase_ != RepetitionPhase::precision)
            return std::unexpected{invalid_transition_error()};
        if (decision == PrecisionStoppingDecision::target_met) {
            complete(state, RepetitionDisposition::precision_target_reached);
        } else if (state.completed_blocks_ >= state.policy_.maximum_blocks()) {
            complete(state, RepetitionDisposition::maximum_blocks_reached);
        } else {
            begin_block(state);
        }
        return state;
    }

    template <typename State> static auto outcome(const State& state) -> RepetitionCompleteAction {
        return RepetitionCompleteAction{*state.outcome_};
    }

    static auto bandwidth_action(const SequentialReadBandwidthRepetitionState& state)
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
            return outcome(state);
        }
        return outcome(state);
    }

    static auto latency_action(const DependentLoadLatencyRepetitionState& state)
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
            return outcome(state);
        }
        return outcome(state);
    }
};

auto create_sequential_read_bandwidth_repetition(
    core::ExperimentId experiment_id, RepetitionPolicy policy,
    ComparisonOrderPolicy comparison_order, bool calibration_enabled, WarmupPolicy warmup_policy,
    std::optional<BandwidthSelectedExecutionState> initial_selected_state)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error> {
    return detail_RepetitionAccess::initialize<SequentialReadBandwidthRepetitionState>(
        experiment_id, std::move(policy), std::move(comparison_order), calibration_enabled,
        std::move(warmup_policy), std::move(initial_selected_state));
}

auto create_dependent_load_latency_repetition(
    core::ExperimentId experiment_id, RepetitionPolicy policy,
    ComparisonOrderPolicy comparison_order, bool calibration_enabled, WarmupPolicy warmup_policy,
    std::optional<LatencySelectedExecutionState> initial_selected_state)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error> {
    return detail_RepetitionAccess::initialize<DependentLoadLatencyRepetitionState>(
        experiment_id, std::move(policy), std::move(comparison_order), calibration_enabled,
        std::move(warmup_policy), std::move(initial_selected_state));
}

auto next_sequential_read_bandwidth_repetition_action(
    const SequentialReadBandwidthRepetitionState& state)
    -> SequentialReadBandwidthRepetitionAction {
    return detail_RepetitionAccess::bandwidth_action(state);
}
auto next_dependent_load_latency_repetition_action(const DependentLoadLatencyRepetitionState& state)
    -> DependentLoadLatencyRepetitionAction {
    return detail_RepetitionAccess::latency_action(state);
}

auto apply_sequential_read_bandwidth_calibration(
    SequentialReadBandwidthRepetitionState state,
    const SequentialReadBandwidthCalibrationOutcome& outcome)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error> {
    std::optional<BandwidthSelectedExecutionState> selected;
    if (const auto candidate = outcome.selected_candidate())
        selected.emplace(
            BandwidthSelectedExecutionState::create(*candidate, outcome.timer_overhead_estimate()));
    return detail_RepetitionAccess::apply_calibration(
        std::move(state), RepetitionPhase::calibration, std::move(selected));
}
auto apply_dependent_load_latency_calibration(DependentLoadLatencyRepetitionState state,
                                              const DependentLoadLatencyCalibrationOutcome& outcome)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error> {
    std::optional<LatencySelectedExecutionState> selected;
    if (const auto candidate = outcome.selected_candidate())
        selected.emplace(
            LatencySelectedExecutionState::create(*candidate, outcome.timer_overhead_estimate()));
    return detail_RepetitionAccess::apply_calibration(
        std::move(state), RepetitionPhase::calibration, std::move(selected));
}
auto apply_sequential_read_bandwidth_warmup(SequentialReadBandwidthRepetitionState state,
                                            const SequentialReadBandwidthWarmupOutcome&)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error> {
    return detail_RepetitionAccess::apply_warmup(std::move(state));
}
auto apply_dependent_load_latency_warmup(DependentLoadLatencyRepetitionState state,
                                         const DependentLoadLatencyWarmupOutcome&)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error> {
    return detail_RepetitionAccess::apply_warmup(std::move(state));
}
auto apply_sequential_read_bandwidth_observation(
    SequentialReadBandwidthRepetitionState state,
    const SequentialReadBandwidthObservation& observation,
    const ObservationValidityEvidence& validity)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error> {
    return detail_RepetitionAccess::apply_observation(std::move(state), observation, validity);
}
auto apply_dependent_load_latency_observation(DependentLoadLatencyRepetitionState state,
                                              const DependentLoadLatencyObservation& observation,
                                              const ObservationValidityEvidence& validity)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error> {
    return detail_RepetitionAccess::apply_observation(std::move(state), observation, validity);
}
auto apply_sequential_read_bandwidth_precision_decision(
    SequentialReadBandwidthRepetitionState state, PrecisionStoppingDecision decision)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error> {
    return detail_RepetitionAccess::apply_precision(std::move(state), decision);
}
auto apply_dependent_load_latency_precision_decision(DependentLoadLatencyRepetitionState state,
                                                     PrecisionStoppingDecision decision)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error> {
    return detail_RepetitionAccess::apply_precision(std::move(state), decision);
}

} // namespace epycbench::measurement
