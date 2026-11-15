#pragma once

#include "epycbench/measurement/block_scheduler.hpp"
#include "epycbench/measurement/repetition.hpp"
#include "epycbench/monitoring/acquisition_driver.hpp"
#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "epycbench/runtime/statistics_production_policy.hpp"
#include "runtime/finalized_evidence_wire_logical_codec.hpp"

#include <cstdint>
#include <array>
#include <algorithm>
#include <charconv>
#include <expected>
#include <string>
#include <span>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace epycbench::runtime::wire_v1::detail {

[[nodiscard]] auto placement_evaluator_domain(const memory::RegionPlacementObservation&) noexcept -> bool;

enum class TypedReconstructionErrorCode : std::uint8_t {
    missing_member,
    invalid_scalar,
    typed_invariant,
    inconsistent_projection,
};

struct TypedReconstructionError final {
    TypedReconstructionErrorCode code;
    std::string path;
    friend auto operator==(const TypedReconstructionError&,
                           const TypedReconstructionError&) noexcept -> bool = default;
};

// Closed component diagnostics use fixed checked assembly. The explicit reference
// adapter alone retains the old unrestricted reference diagnostic behavior.
class ReconstructionPath final {
  public:
    static constexpr std::size_t fixed_capacity = 512;
    ReconstructionPath() = default;
    [[nodiscard]] static auto reference(std::string_view text) -> ReconstructionPath {
        ReconstructionPath result;
        result.reference_ = true;
        result.append(text);
        return result;
    }
    ReconstructionPath(std::string_view text) { append(text); }
    ReconstructionPath(const char* text) : ReconstructionPath(std::string_view{text}) {}
    ReconstructionPath(const std::string& text) : ReconstructionPath(std::string_view{text}) {}
    [[nodiscard]] auto view() const noexcept -> std::string_view {
        return dynamic_ ? std::string_view{overflow_} : std::string_view{fixed_.data(), size_};
    }
    [[nodiscard]] auto fixed() const noexcept -> bool { return !dynamic_; }
    [[nodiscard]] auto overflowed() const noexcept -> bool { return overflowed_; }
    [[nodiscard]] auto render() const -> std::string { return std::string{view()}; }
    void append(std::string_view text) {
        if (overflowed_) return;
        if (dynamic_) { overflow_.append(text); return; }
        if (text.size() <= fixed_.size() - size_) {
            std::copy(text.begin(), text.end(), fixed_.begin() + static_cast<std::ptrdiff_t>(size_));
            size_ += text.size();
        } else {
            if (!reference_) { overflowed_ = true; return; }
            overflow_.assign(fixed_.data(), size_);
            overflow_.append(text);
            dynamic_ = true;
        }
    }
    [[nodiscard]] static auto ordinal(std::uint64_t value) noexcept -> ReconstructionPath {
        ReconstructionPath result;
        const auto converted = std::to_chars(result.fixed_.data(),
            result.fixed_.data() + result.fixed_.size(), value);
        result.size_ = static_cast<std::size_t>(converted.ptr - result.fixed_.data());
        return result;
    }
    friend auto operator+(ReconstructionPath left, std::string_view right) -> ReconstructionPath {
        left.append(right); return left;
    }
    friend auto operator+(ReconstructionPath left, const ReconstructionPath& right)
        -> ReconstructionPath { left.append(right.view()); return left; }
    friend auto operator+(const char* left, const ReconstructionPath& right)
        -> ReconstructionPath { auto value = ReconstructionPath{left}; value.append(right.view()); return value; }
    friend auto operator+(ReconstructionPath left, const std::string& right) -> ReconstructionPath {
        left.append(right); return left;
    }
    friend auto operator+(ReconstructionPath left, const char* right) -> ReconstructionPath {
        left.append(right); return left;
    }
  private:
    std::array<char, fixed_capacity> fixed_{};
    std::size_t size_{};
    std::string overflow_;
    bool dynamic_{};
    bool reference_{};
    bool overflowed_{};
};
struct FixedTypedReconstructionError final {
    TypedReconstructionErrorCode code;
    ReconstructionPath path;
};

// Slice-one owning value. It deliberately cannot construct the finalized root:
// later subsystem slices must reconstruct every remaining owning branch first.
struct ReconstructedEvidenceHeader final {
    PairwiseRunProvenance provenance;
    StatisticsProductionPolicy statistics_policy;
    monitoring::MonitoringExecutionPolicy monitoring_policy;
    measurement::RepetitionPolicy repetition_policy;
    measurement::ComparisonOrderPolicy comparison_order;
};

// Slice-two owning value. These members remain separate from the finalized
// root until every sibling evidence branch has been reconstructed.
struct ReconstructedCalibrationWarmup final {
    std::optional<PairwiseCalibrationEvidence> calibration;
    std::optional<PairwiseWarmupEvidence> warmup;
};

// Slice-three owner. Failed-observation errors, stopping, terminal, and
// finalization remain deliberately outside this owner and therefore it cannot
// construct the finalized root.
struct ReconstructedObservationBlock final {
    measurement::MeasurementBlockPlan plan;
    std::vector<FactualObservationEvidence> observations;
    bool failed_observation_present;
    BlockAttemptCompletion stated_completion;
};

struct ReconstructedObservationBlocks final {
    std::vector<ReconstructedObservationBlock> blocks;
};

// Slice-four owner.  It enriches the slice-three block values with the exact
// failure payload and separately owns every stopping-boundary evaluation.  It
// still cannot construct a terminal or the finalized evidence root.
struct ReconstructedFailedObservation final {
    std::size_t block_index;
    FailedObservationEvidence evidence;
};

struct ReconstructedStoppingAndFailures final {
    std::vector<RunnerStoppingEvaluationEvidence> stopping_evaluations;
    std::vector<ReconstructedFailedObservation> failed_observations;
};

// Slice-five owner. It binds terminal/finalization projections to the prior
// block/failure/stopping slices but deliberately does not construct the root.
struct ReconstructedTerminalAndFinalization final {
    PairwiseRunTerminalEvidence terminal;
    std::optional<PairwiseFinalizationEvidence> finalization;
};

struct ComponentProjectionCount;
[[nodiscard]] auto preparation_outcome_reconstruction_staging() noexcept -> std::array<std::size_t,6>;
[[nodiscard]] auto fixed_reconstruct_calibration_component(const nlohmann::json&)
    -> std::expected<PairwiseCalibrationEvidence, FixedTypedReconstructionError>;
[[nodiscard]] auto fixed_reconstruct_warmup_component(const nlohmann::json&)
    -> std::expected<PairwiseWarmupEvidence, FixedTypedReconstructionError>;
[[nodiscard]] auto count_calibration_component(const PairwiseCalibrationEvidence&) -> ComponentProjectionCount;
[[nodiscard]] auto project_calibration_component(const PairwiseCalibrationEvidence&) -> nlohmann::json;
[[nodiscard]] auto count_warmup_component(const PairwiseWarmupEvidence&) -> ComponentProjectionCount;
[[nodiscard]] auto project_warmup_component(const PairwiseWarmupEvidence&) -> nlohmann::json;
[[nodiscard]] auto count_block_plan_component(const measurement::MeasurementBlockPlan&) -> ComponentProjectionCount;
[[nodiscard]] auto project_block_plan_component(const measurement::MeasurementBlockPlan&) -> nlohmann::json;
[[nodiscard]] auto count_factual_component(const FactualObservationEvidence&) -> ComponentProjectionCount;
[[nodiscard]] auto project_factual_component(const FactualObservationEvidence&) -> nlohmann::json;
[[nodiscard]] auto count_failed_component(const FailedObservationEvidence&) -> ComponentProjectionCount;
[[nodiscard]] auto project_failed_component(const FailedObservationEvidence&) -> nlohmann::json;

struct StoppingPrefixCounts final {
    std::uint64_t boundary{}, completed{}, valid{};
    bool boundary_found{};
};
[[nodiscard]] auto fixed_reconstruct_stopping_component(const nlohmann::json&,
    const StatisticsProductionPolicy&, const measurement::RepetitionPolicy&,
    StoppingPrefixCounts, std::optional<std::uint64_t>, ReconstructionPath)
    -> std::expected<RunnerStoppingEvaluationEvidence, FixedTypedReconstructionError>;
[[nodiscard]] auto count_stopping_component(const RunnerStoppingEvaluationEvidence&) -> ComponentProjectionCount;
[[nodiscard]] auto project_stopping_component(const RunnerStoppingEvaluationEvidence&) -> nlohmann::json;

// Requires registry-admitted nonnull finalization shape. Absent error count defers
// only terminal reference bounds; all local Memory and ordinal invariants remain.
[[nodiscard]] auto fixed_reconstruct_finalization_component(const nlohmann::json&,
    std::optional<std::uint64_t> operational_error_count)
    -> std::expected<PairwiseFinalizationEvidence, FixedTypedReconstructionError>;
[[nodiscard]] auto validate_normal_finalization(const PairwiseFinalizationEvidence&)
    -> std::expected<void, FixedTypedReconstructionError>;

// Header and worker phases preserve the root consumer's identity-error precedence.
// These reference operations do not seal a component or assert root admission.
struct ReconstructedFailedComponentHeader final {
    measurement::ScheduledObservationSlot slot;
    core::ObservationId observation_id;
    FinalizedEvidenceError primary_error;
};
[[nodiscard]] auto reconstruct_failed_component_header(
    const nlohmann::json&, const measurement::MeasurementBlockPlan&, std::string_view path)
    -> std::expected<ReconstructedFailedComponentHeader, TypedReconstructionError>;
[[nodiscard]] auto fixed_reconstruct_failed_component_header(
    const nlohmann::json&, const measurement::MeasurementBlockPlan&, ReconstructionPath path)
    -> std::expected<ReconstructedFailedComponentHeader, FixedTypedReconstructionError>;
[[nodiscard]] auto reconstruct_failed_component_workers(
    const nlohmann::json&, ReconstructedFailedComponentHeader, std::string_view path)
    -> std::expected<FailedObservationEvidence, TypedReconstructionError>;
[[nodiscard]] auto fixed_reconstruct_failed_component_workers(
    const nlohmann::json&, ReconstructedFailedComponentHeader, ReconstructionPath path)
    -> std::expected<FailedObservationEvidence, FixedTypedReconstructionError>;

// Shared named registry components; callers retain root/cross-block obligations.
[[nodiscard]] auto reconstruct_block_plan_component(const nlohmann::json&)
    -> std::expected<measurement::MeasurementBlockPlan, TypedReconstructionError>;
[[nodiscard]] auto fixed_reconstruct_block_plan_component(const nlohmann::json&)
    -> std::expected<measurement::MeasurementBlockPlan, FixedTypedReconstructionError>;
[[nodiscard]] auto reconstruct_factual_component(
    const nlohmann::json&, const measurement::MeasurementBlockPlan&, std::size_t expected_slot,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FactualObservationEvidence, TypedReconstructionError>;
[[nodiscard]] auto fixed_reconstruct_factual_component(
    const nlohmann::json&, const measurement::MeasurementBlockPlan&, std::size_t expected_slot,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FactualObservationEvidence, FixedTypedReconstructionError>;

[[nodiscard]] auto reconstruct_evidence_header(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedEvidenceHeader, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_evidence_header(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedEvidenceHeader, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_calibration_warmup(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedCalibrationWarmup, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_calibration_warmup(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedCalibrationWarmup, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_observation_blocks(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_observation_blocks(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_observation_blocks(
    const nlohmann::json& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_stopping_and_failures(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedStoppingAndFailures, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_stopping_and_failures(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedStoppingAndFailures, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_finalized_error_value(const nlohmann::json& bounded_error)
    -> std::expected<FinalizedEvidenceError, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_terminal_and_finalization(const nlohmann::json& bounded_body)
    -> std::expected<ReconstructedTerminalAndFinalization, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_terminal_and_finalization(const LogicalBody& bounded_body)
    -> std::expected<ReconstructedTerminalAndFinalization, TypedReconstructionError>;
[[nodiscard]] auto reconstruct_observation_blocks(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<ReconstructedObservationBlocks, TypedReconstructionError>;

// Final inactive reconstruction slice.  This composes the independently
// reconstructed owners through the real production ledger so the same
// structural state-machine validation used by the selected in-process graph
// is applied to admitted wire evidence.
[[nodiscard]] auto reconstruct_finalized_evidence(
    const LogicalBody& bounded_body,
    std::span<const core::LinuxCpuId> authenticated_cpus)
    -> std::expected<FinalizedPairwiseRunEvidence, TypedReconstructionError>;

// Inactive typed projection counterpart. The returned bytes are the exact
// Core-canonical logical body admitted by the registry-specific decoder. It
// does not select a protocol, frame, route, process, or live authority.
[[nodiscard]] auto count_finalized_evidence(
    const FinalizedPairwiseRunEvidence&) -> ComponentProjectionCount;

[[nodiscard]] auto encode_finalized_evidence(
    const FinalizedPairwiseRunEvidence& evidence,
    const CalculatedEvidenceCapacity& capacity) noexcept
    -> std::expected<std::string, LogicalCodecError>;

} // namespace epycbench::runtime::wire_v1::detail
