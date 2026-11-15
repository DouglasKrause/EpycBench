#pragma once

#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"
#include "runtime/pairwise_typed_adapter_storage.hpp"
#include "runtime/pairwise_typed_operation_source.hpp"

#include <span>

namespace epycbench::runtime::detail {

enum class PairwiseCoreOperation : std::uint8_t {
    calibrate_bandwidth,
    calibrate_latency,
    warmup_bandwidth,
    warmup_latency,
    observe_bandwidth,
    observe_latency,
    finalize,
    count,
};

enum class PairwiseRunState : std::uint8_t {
    active,
    failed,
    finalized,
    moved_from,
};

struct PairwiseFinalizationSource final {
    PairwiseFinalizationEvidence evidence;
    std::span<const LocalFinalizationErrorAssociation> associations;
    std::span<PairwiseEvidenceFailure> errors;
};

struct PairwiseProductionRunFunctions final {
    PairwiseTypedOperationSourceResult<measurement::SequentialReadBandwidthCalibrationOutcome>
        (*calibrate_bandwidth)(void*, const measurement::CalibrationDurationPolicy&) noexcept{};
    PairwiseTypedOperationSourceResult<measurement::DependentLoadLatencyCalibrationOutcome>
        (*calibrate_latency)(void*, const measurement::CalibrationDurationPolicy&) noexcept{};
    PairwiseTypedOperationSourceResult<measurement::SequentialReadBandwidthWarmupOutcome>
        (*warmup_bandwidth)(void*, const measurement::WarmupPolicy&,
                            kernel::SequentialReadBandwidthPassCount) noexcept{};
    PairwiseTypedOperationSourceResult<measurement::DependentLoadLatencyWarmupOutcome>
        (*warmup_latency)(void*, const measurement::WarmupPolicy&,
                          kernel::DependentLoadLatencyStepCount) noexcept{};
    PairwiseTypedOperationSourceResult<FactualObservationEvidence>
        (*observe_bandwidth)(void*, const measurement::SequentialReadBandwidthObservationRequiredAction&) noexcept{};
    PairwiseTypedOperationSourceResult<FactualObservationEvidence>
        (*observe_latency)(void*, const measurement::DependentLoadLatencyObservationRequiredAction&) noexcept{};
    PairwiseFinalizationSource (*finalize)(void*) noexcept{};
};

class PairwiseProductionRunCore final {
  public:
    PairwiseProductionRunCore(PairwiseProductionRunCore&&) noexcept;
    PairwiseProductionRunCore(const PairwiseProductionRunCore&) = delete;
    auto operator=(PairwiseProductionRunCore&&) -> PairwiseProductionRunCore& = delete;
    auto operator=(const PairwiseProductionRunCore&) -> PairwiseProductionRunCore& = delete;

    [[nodiscard]] auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy&) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome>;
    [[nodiscard]] auto calibrate_latency(const measurement::CalibrationDurationPolicy&) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyCalibrationOutcome>;
    [[nodiscard]] auto warmup_bandwidth(const measurement::WarmupPolicy&,
                                        kernel::SequentialReadBandwidthPassCount) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthWarmupOutcome>;
    [[nodiscard]] auto warmup_latency(const measurement::WarmupPolicy&,
                                      kernel::DependentLoadLatencyStepCount) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyWarmupOutcome>;
    [[nodiscard]] auto observe_bandwidth(
        const measurement::SequentialReadBandwidthObservationRequiredAction&) noexcept
        -> PairwiseOperationResult<FactualObservationEvidence>;
    [[nodiscard]] auto observe_latency(
        const measurement::DependentLoadLatencyObservationRequiredAction&) noexcept
        -> PairwiseOperationResult<FactualObservationEvidence>;
    [[nodiscard]] auto finalize() && noexcept -> PairwiseFinalizationOutcome;

  private:
    PairwiseProductionRunCore(PairwiseTypedPreparedOwners&&,
                              PairwiseProductionRunFunctions) noexcept;
    void require_progression() const noexcept;
    template <class T, class Invoke>
    [[nodiscard]] auto invoke_operation(PairwiseCoreOperation, Invoke&&) noexcept
        -> PairwiseOperationResult<T>;
    [[nodiscard]] auto materialize_failure(
        PairwiseCoreOperation, PairwiseTypedOperationFailureSource) noexcept
        -> PairwiseOperationFailure;
    [[nodiscard]] auto take_failure_values(
        PairwiseTypedAdapterStorage&, PairwiseTypedFailureDestination) noexcept
        -> std::vector<PairwiseEvidenceFailure>;

    PairwiseTypedPreparedOwners owners_;
    PairwiseProductionRunFunctions functions_;
    PairwiseRunState state_{PairwiseRunState::active};
    friend auto prepare_pairwise_production_run(
        PairwiseTypedPreparedOwners&&, PairwiseProductionRunFunctions) noexcept
        -> PairwiseOperationResult<PairwiseProductionRunCore>;
};

[[nodiscard]] auto materialize_pairwise_owner_preparation_failure(
    PairwiseTypedReleasedOwnerPreparationFailure&&) noexcept
    -> PairwiseOperationFailure;

[[nodiscard]] auto prepare_pairwise_production_run(
    PairwiseTypedPreparedOwners&&, PairwiseProductionRunFunctions) noexcept
    -> PairwiseOperationResult<PairwiseProductionRunCore>;
// The wrapper allocation is separate from installing an actually acquired core.
class PreparedPairwiseProductionRun final {
  public:
    PreparedPairwiseProductionRun(PreparedPairwiseProductionRun&&) noexcept = default;
    PreparedPairwiseProductionRun(const PreparedPairwiseProductionRun&) = delete;
    auto operator=(PreparedPairwiseProductionRun&&) -> PreparedPairwiseProductionRun& = delete;
    [[nodiscard]] static auto auxiliary_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto prepare() noexcept
        -> PairwiseOperationResult<PreparedPairwiseProductionRun>;
    [[nodiscard]] auto bind(PairwiseProductionRunCore&&) && noexcept
        -> PairwiseOperationResult<PairwiseProductionRun>;
  private:
    explicit PreparedPairwiseProductionRun(
        std::unique_ptr<PairwiseProductionRun::Implementation> value) noexcept
        : destination_{std::move(value)} {}
    std::unique_ptr<PairwiseProductionRun::Implementation> destination_;
};

[[nodiscard]] auto make_pairwise_production_run(
    PairwiseProductionRunCore&&) noexcept
    -> PairwiseOperationResult<PairwiseProductionRun>;
[[nodiscard]] auto prepare_pairwise_production_run(
    const PairwiseProductionOperations&,
    const AdmittedPairwiseProductionInputs&) noexcept
    -> PairwiseOperationResult<PairwiseProductionRun>;

} // namespace epycbench::runtime::detail
