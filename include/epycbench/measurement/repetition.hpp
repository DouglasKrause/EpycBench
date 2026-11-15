#pragma once

#include "epycbench/measurement/block_scheduler.hpp"
#include "epycbench/measurement/calibration.hpp"
#include "epycbench/measurement/observation.hpp"
#include "epycbench/measurement/validity.hpp"
#include "epycbench/measurement/warmup.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <variant>

namespace epycbench::measurement {

namespace detail {
struct MeasurementLeafAccess;
}

class RepetitionPolicy {
  public:
    RepetitionPolicy(const RepetitionPolicy&) noexcept = default;
    RepetitionPolicy(RepetitionPolicy&&) noexcept = default;
    auto operator=(const RepetitionPolicy&) -> RepetitionPolicy& = delete;
    auto operator=(RepetitionPolicy&&) -> RepetitionPolicy& = delete;

    [[nodiscard]] static auto create(std::uint64_t minimum_blocks, std::uint64_t maximum_blocks,
                                     std::uint64_t minimum_valid_observations_per_variant,
                                     bool stopping_enabled)
        -> std::expected<RepetitionPolicy, core::Error>;
    [[nodiscard]] auto minimum_blocks() const noexcept -> std::uint64_t;
    [[nodiscard]] auto maximum_blocks() const noexcept -> std::uint64_t;
    [[nodiscard]] auto minimum_valid_observations_per_variant() const noexcept -> std::uint64_t;
    [[nodiscard]] auto stopping_enabled() const noexcept -> bool;
    auto operator==(const RepetitionPolicy&) const -> bool = default;

  private:
    friend struct detail::MeasurementLeafAccess;
    RepetitionPolicy(std::uint64_t minimum_blocks, std::uint64_t maximum_blocks,
                     std::uint64_t minimum_valid_observations_per_variant,
                     bool stopping_enabled) noexcept;
    std::uint64_t minimum_blocks_;
    std::uint64_t maximum_blocks_;
    std::uint64_t minimum_valid_observations_per_variant_;
    bool stopping_enabled_;
};

enum class PrecisionStoppingDecision { target_met, target_not_met, unavailable };
enum class RepetitionDisposition {
    precision_target_reached,
    maximum_blocks_reached,
    selected_work_unavailable,
};

class BandwidthSelectedExecutionState {
  public:
    [[nodiscard]] static auto create(kernel::SequentialReadBandwidthPassCount selected_work,
                                     TimerOverheadEstimate timer_overhead_estimate) noexcept
        -> BandwidthSelectedExecutionState;
    [[nodiscard]] auto selected_work() const noexcept -> kernel::SequentialReadBandwidthPassCount;
    [[nodiscard]] auto timer_overhead_estimate() const noexcept -> TimerOverheadEstimate;

  private:
    BandwidthSelectedExecutionState(kernel::SequentialReadBandwidthPassCount selected_work,
                                    TimerOverheadEstimate timer_overhead_estimate) noexcept;
    kernel::SequentialReadBandwidthPassCount selected_work_;
    TimerOverheadEstimate timer_overhead_estimate_;
};

class LatencySelectedExecutionState {
  public:
    [[nodiscard]] static auto create(kernel::DependentLoadLatencyStepCount selected_work,
                                     TimerOverheadEstimate timer_overhead_estimate) noexcept
        -> LatencySelectedExecutionState;
    [[nodiscard]] auto selected_work() const noexcept -> kernel::DependentLoadLatencyStepCount;
    [[nodiscard]] auto timer_overhead_estimate() const noexcept -> TimerOverheadEstimate;

  private:
    LatencySelectedExecutionState(kernel::DependentLoadLatencyStepCount selected_work,
                                  TimerOverheadEstimate timer_overhead_estimate) noexcept;
    kernel::DependentLoadLatencyStepCount selected_work_;
    TimerOverheadEstimate timer_overhead_estimate_;
};

class RepetitionOutcome {
  public:
    [[nodiscard]] auto experiment_id() const noexcept -> core::ExperimentId;
    [[nodiscard]] auto policy() const noexcept -> const RepetitionPolicy&;
    [[nodiscard]] auto disposition() const noexcept -> RepetitionDisposition;
    [[nodiscard]] auto completed_blocks() const noexcept -> std::uint64_t;
    [[nodiscard]] auto complete_valid_blocks() const noexcept -> std::uint64_t;
    [[nodiscard]] auto valid_first_variant_observations() const noexcept -> std::uint64_t;
    [[nodiscard]] auto valid_second_variant_observations() const noexcept -> std::uint64_t;

  private:
    RepetitionOutcome(core::ExperimentId experiment_id, RepetitionPolicy policy,
                      RepetitionDisposition disposition, std::uint64_t completed_blocks,
                      std::uint64_t complete_valid_blocks,
                      std::uint64_t valid_first_variant_observations,
                      std::uint64_t valid_second_variant_observations) noexcept;
    core::ExperimentId experiment_id_;
    RepetitionPolicy policy_;
    RepetitionDisposition disposition_;
    std::uint64_t completed_blocks_;
    std::uint64_t complete_valid_blocks_;
    std::uint64_t valid_first_variant_observations_;
    std::uint64_t valid_second_variant_observations_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

struct SequentialReadBandwidthCalibrationRequiredAction {
    auto operator==(const SequentialReadBandwidthCalibrationRequiredAction&) const
        -> bool = default;
};
struct DependentLoadLatencyCalibrationRequiredAction {
    auto operator==(const DependentLoadLatencyCalibrationRequiredAction&) const -> bool = default;
};

class SequentialReadBandwidthWarmupRequiredAction {
  public:
    [[nodiscard]] auto selected_state() const noexcept -> const BandwidthSelectedExecutionState&;

  private:
    explicit SequentialReadBandwidthWarmupRequiredAction(
        BandwidthSelectedExecutionState selected_state) noexcept;
    BandwidthSelectedExecutionState selected_state_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

class DependentLoadLatencyWarmupRequiredAction {
  public:
    [[nodiscard]] auto selected_state() const noexcept -> const LatencySelectedExecutionState&;

  private:
    explicit DependentLoadLatencyWarmupRequiredAction(
        LatencySelectedExecutionState selected_state) noexcept;
    LatencySelectedExecutionState selected_state_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

class SequentialReadBandwidthObservationRequiredAction {
  public:
    [[nodiscard]] auto experiment_id() const noexcept -> core::ExperimentId;
    [[nodiscard]] auto observation_id() const noexcept -> core::ObservationId;
    [[nodiscard]] auto block_plan() const noexcept -> const MeasurementBlockPlan&;
    [[nodiscard]] auto slot() const noexcept -> const ScheduledObservationSlot&;
    [[nodiscard]] auto selected_state() const noexcept -> const BandwidthSelectedExecutionState&;

  private:
    SequentialReadBandwidthObservationRequiredAction(
        core::ExperimentId experiment_id, core::ObservationId observation_id,
        MeasurementBlockPlan block_plan, std::uint8_t slot_index,
        BandwidthSelectedExecutionState selected_state) noexcept;
    core::ExperimentId experiment_id_;
    core::ObservationId observation_id_;
    MeasurementBlockPlan block_plan_;
    std::uint8_t slot_index_;
    BandwidthSelectedExecutionState selected_state_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

class DependentLoadLatencyObservationRequiredAction {
  public:
    [[nodiscard]] auto experiment_id() const noexcept -> core::ExperimentId;
    [[nodiscard]] auto observation_id() const noexcept -> core::ObservationId;
    [[nodiscard]] auto block_plan() const noexcept -> const MeasurementBlockPlan&;
    [[nodiscard]] auto slot() const noexcept -> const ScheduledObservationSlot&;
    [[nodiscard]] auto selected_state() const noexcept -> const LatencySelectedExecutionState&;

  private:
    DependentLoadLatencyObservationRequiredAction(
        core::ExperimentId experiment_id, core::ObservationId observation_id,
        MeasurementBlockPlan block_plan, std::uint8_t slot_index,
        LatencySelectedExecutionState selected_state) noexcept;
    core::ExperimentId experiment_id_;
    core::ObservationId observation_id_;
    MeasurementBlockPlan block_plan_;
    std::uint8_t slot_index_;
    LatencySelectedExecutionState selected_state_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

class PrecisionEvaluationRequiredAction {
  public:
    [[nodiscard]] auto completed_blocks() const noexcept -> std::uint64_t;
    [[nodiscard]] auto complete_valid_blocks() const noexcept -> std::uint64_t;

  private:
    PrecisionEvaluationRequiredAction(std::uint64_t completed_blocks,
                                      std::uint64_t complete_valid_blocks) noexcept;
    std::uint64_t completed_blocks_;
    std::uint64_t complete_valid_blocks_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

class RepetitionCompleteAction {
  public:
    [[nodiscard]] auto outcome() const noexcept -> const RepetitionOutcome&;

  private:
    explicit RepetitionCompleteAction(RepetitionOutcome outcome) noexcept;
    RepetitionOutcome outcome_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

using SequentialReadBandwidthRepetitionAction =
    std::variant<SequentialReadBandwidthCalibrationRequiredAction,
                 SequentialReadBandwidthWarmupRequiredAction,
                 SequentialReadBandwidthObservationRequiredAction,
                 PrecisionEvaluationRequiredAction, RepetitionCompleteAction>;
using DependentLoadLatencyRepetitionAction =
    std::variant<DependentLoadLatencyCalibrationRequiredAction,
                 DependentLoadLatencyWarmupRequiredAction,
                 DependentLoadLatencyObservationRequiredAction, PrecisionEvaluationRequiredAction,
                 RepetitionCompleteAction>;

enum class RepetitionPhase { calibration, warmup, observation, precision, complete };

template <typename SelectedState> class BasicRepetitionState {
  public:
    BasicRepetitionState(const BasicRepetitionState&) = default;
    BasicRepetitionState(BasicRepetitionState&&) noexcept = default;
    auto operator=(const BasicRepetitionState&) -> BasicRepetitionState& = delete;
    auto operator=(BasicRepetitionState&&) -> BasicRepetitionState& = delete;

    [[nodiscard]] auto experiment_id() const noexcept -> core::ExperimentId {
        return experiment_id_;
    }
    [[nodiscard]] auto policy() const noexcept -> const RepetitionPolicy& { return policy_; }
    [[nodiscard]] auto comparison_order() const noexcept -> const ComparisonOrderPolicy& {
        return comparison_order_;
    }
    [[nodiscard]] auto selected_state() const noexcept -> const std::optional<SelectedState>& {
        return selected_state_;
    }
    [[nodiscard]] auto completed_blocks() const noexcept -> std::uint64_t {
        return completed_blocks_;
    }
    [[nodiscard]] auto complete_valid_blocks() const noexcept -> std::uint64_t {
        return complete_valid_blocks_;
    }
    [[nodiscard]] auto valid_first_variant_observations() const noexcept -> std::uint64_t {
        return valid_first_variant_observations_;
    }
    [[nodiscard]] auto valid_second_variant_observations() const noexcept -> std::uint64_t {
        return valid_second_variant_observations_;
    }
    [[nodiscard]] auto phase() const noexcept -> RepetitionPhase { return phase_; }

  private:
    BasicRepetitionState(core::ExperimentId experiment_id, RepetitionPolicy policy,
                         ComparisonOrderPolicy comparison_order, bool calibration_enabled,
                         WarmupPolicy warmup_policy,
                         std::optional<SelectedState> selected_state) noexcept
        : experiment_id_{experiment_id}, policy_{std::move(policy)},
          comparison_order_{std::move(comparison_order)}, calibration_enabled_{calibration_enabled},
          warmup_policy_{std::move(warmup_policy)}, selected_state_{std::move(selected_state)} {}

    core::ExperimentId experiment_id_;
    RepetitionPolicy policy_;
    ComparisonOrderPolicy comparison_order_;
    bool calibration_enabled_;
    WarmupPolicy warmup_policy_;
    std::optional<SelectedState> selected_state_;
    std::uint64_t next_block_id_{0U};
    std::uint64_t next_observation_id_{0U};
    std::optional<MeasurementBlockPlan> active_plan_;
    std::uint8_t active_slot_{0U};
    std::array<ObservationValidityDisposition, 4> block_validity_{
        ObservationValidityDisposition::invalid, ObservationValidityDisposition::invalid,
        ObservationValidityDisposition::invalid, ObservationValidityDisposition::invalid};
    std::uint64_t completed_blocks_{0U};
    std::uint64_t complete_valid_blocks_{0U};
    std::uint64_t valid_first_variant_observations_{0U};
    std::uint64_t valid_second_variant_observations_{0U};
    bool calibration_requested_{false};
    bool warmup_requested_{false};
    RepetitionPhase phase_{RepetitionPhase::complete};
    std::optional<RepetitionOutcome> outcome_;
    friend class detail_RepetitionAccess;
    friend struct detail::MeasurementLeafAccess;
};

using SequentialReadBandwidthRepetitionState =
    BasicRepetitionState<BandwidthSelectedExecutionState>;
using DependentLoadLatencyRepetitionState = BasicRepetitionState<LatencySelectedExecutionState>;

[[nodiscard]] auto create_sequential_read_bandwidth_repetition(
    core::ExperimentId experiment_id, RepetitionPolicy policy,
    ComparisonOrderPolicy comparison_order, bool calibration_enabled, WarmupPolicy warmup_policy,
    std::optional<BandwidthSelectedExecutionState> initial_selected_state = std::nullopt)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error>;
[[nodiscard]] auto create_dependent_load_latency_repetition(
    core::ExperimentId experiment_id, RepetitionPolicy policy,
    ComparisonOrderPolicy comparison_order, bool calibration_enabled, WarmupPolicy warmup_policy,
    std::optional<LatencySelectedExecutionState> initial_selected_state = std::nullopt)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error>;

[[nodiscard]] auto next_sequential_read_bandwidth_repetition_action(
    const SequentialReadBandwidthRepetitionState& state) -> SequentialReadBandwidthRepetitionAction;
[[nodiscard]] auto
next_dependent_load_latency_repetition_action(const DependentLoadLatencyRepetitionState& state)
    -> DependentLoadLatencyRepetitionAction;

[[nodiscard]] auto apply_sequential_read_bandwidth_calibration(
    SequentialReadBandwidthRepetitionState state,
    const SequentialReadBandwidthCalibrationOutcome& outcome)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error>;
[[nodiscard]] auto
apply_dependent_load_latency_calibration(DependentLoadLatencyRepetitionState state,
                                         const DependentLoadLatencyCalibrationOutcome& outcome)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error>;
[[nodiscard]] auto
apply_sequential_read_bandwidth_warmup(SequentialReadBandwidthRepetitionState state,
                                       const SequentialReadBandwidthWarmupOutcome& outcome)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error>;
[[nodiscard]] auto
apply_dependent_load_latency_warmup(DependentLoadLatencyRepetitionState state,
                                    const DependentLoadLatencyWarmupOutcome& outcome)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error>;
[[nodiscard]] auto
apply_sequential_read_bandwidth_observation(SequentialReadBandwidthRepetitionState state,
                                            const SequentialReadBandwidthObservation& observation,
                                            const ObservationValidityEvidence& validity)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error>;
[[nodiscard]] auto
apply_dependent_load_latency_observation(DependentLoadLatencyRepetitionState state,
                                         const DependentLoadLatencyObservation& observation,
                                         const ObservationValidityEvidence& validity)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error>;
[[nodiscard]] auto
apply_sequential_read_bandwidth_precision_decision(SequentialReadBandwidthRepetitionState state,
                                                   PrecisionStoppingDecision decision)
    -> std::expected<SequentialReadBandwidthRepetitionState, core::Error>;
[[nodiscard]] auto
apply_dependent_load_latency_precision_decision(DependentLoadLatencyRepetitionState state,
                                                PrecisionStoppingDecision decision)
    -> std::expected<DependentLoadLatencyRepetitionState, core::Error>;

} // namespace epycbench::measurement
