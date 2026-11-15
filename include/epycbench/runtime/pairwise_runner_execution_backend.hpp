#pragma once

#include "epycbench/runtime/production_admission.hpp"
#include "epycbench/runtime/pairwise_run_evidence.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace epycbench::runtime {

[[nodiscard]] auto production_calibration_policy(
    const AdmittedPairwiseProductionInputs& inputs)
    -> std::expected<measurement::CalibrationDurationPolicy, core::Error>;

struct PairwiseOperationFailure final {
    PairwiseEvidenceFailure primary;
    std::vector<PairwiseEvidenceFailure> secondary;
    std::optional<kernel::DependentLoadLatencyBlockId> active_block_id{};
};

template <typename T>
using PairwiseOperationResult = std::expected<T, PairwiseOperationFailure>;

namespace detail {
struct LocalFinalizationErrorAssociation final {
    measurement::ComparisonVariantSlot slot;
    std::size_t worker_index;
    std::optional<std::uint64_t> primary_error_ordinal;
    std::optional<std::uint64_t> cleanup_error_ordinal;
};
} // namespace detail

struct PairwiseFinalizationOutcome final {
    PairwiseFinalizationOutcome(
        PairwiseFinalizationEvidence owned_evidence,
        std::vector<detail::LocalFinalizationErrorAssociation> owned_associations,
        std::vector<PairwiseEvidenceFailure> owned_errors) noexcept
        : evidence{std::move(owned_evidence)},
          associations{std::move(owned_associations)},
          errors{std::move(owned_errors)} {}
    PairwiseFinalizationOutcome(PairwiseFinalizationOutcome&&) noexcept = default;
    PairwiseFinalizationOutcome(const PairwiseFinalizationOutcome&) = delete;
    auto operator=(PairwiseFinalizationOutcome&&) noexcept
        -> PairwiseFinalizationOutcome& = default;
    auto operator=(const PairwiseFinalizationOutcome&)
        -> PairwiseFinalizationOutcome& = delete;

    PairwiseFinalizationEvidence evidence;
    std::vector<detail::LocalFinalizationErrorAssociation> associations;
    std::vector<PairwiseEvidenceFailure> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return errors.empty(); }
};

class PairwiseProductionRun {
  public:
    class Implementation;

    explicit PairwiseProductionRun(std::unique_ptr<Implementation>) noexcept;
    PairwiseProductionRun(PairwiseProductionRun&&) noexcept;
    PairwiseProductionRun(const PairwiseProductionRun&) = delete;
    auto operator=(PairwiseProductionRun&&) -> PairwiseProductionRun& = delete;
    auto operator=(const PairwiseProductionRun&) -> PairwiseProductionRun& = delete;
    ~PairwiseProductionRun() noexcept;

    [[nodiscard]] auto calibrate_bandwidth(
        const measurement::CalibrationDurationPolicy&) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome>;
    [[nodiscard]] auto calibrate_latency(
        const measurement::CalibrationDurationPolicy&) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyCalibrationOutcome>;
    [[nodiscard]] auto warmup_bandwidth(
        const measurement::WarmupPolicy&,
        kernel::SequentialReadBandwidthPassCount) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthWarmupOutcome>;
    [[nodiscard]] auto warmup_latency(
        const measurement::WarmupPolicy&,
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
    std::unique_ptr<Implementation> implementation_;
};

class PairwiseProductionRun::Implementation {
  public:
    virtual ~Implementation() noexcept = default;
    virtual auto calibrate_bandwidth(
        const measurement::CalibrationDurationPolicy&) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome> = 0;
    virtual auto calibrate_latency(
        const measurement::CalibrationDurationPolicy&) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyCalibrationOutcome> = 0;
    virtual auto warmup_bandwidth(
        const measurement::WarmupPolicy&,
        kernel::SequentialReadBandwidthPassCount) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthWarmupOutcome> = 0;
    virtual auto warmup_latency(
        const measurement::WarmupPolicy&,
        kernel::DependentLoadLatencyStepCount) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyWarmupOutcome> = 0;
    virtual auto observe_bandwidth(
        const measurement::SequentialReadBandwidthObservationRequiredAction&) noexcept
        -> PairwiseOperationResult<FactualObservationEvidence> = 0;
    virtual auto observe_latency(
        const measurement::DependentLoadLatencyObservationRequiredAction&) noexcept
        -> PairwiseOperationResult<FactualObservationEvidence> = 0;
    virtual auto finalize_pairwise() noexcept -> PairwiseFinalizationOutcome = 0;
};

struct PairwiseProductionOperations final {
    PairwiseProductionOperations() noexcept = default;
    PairwiseProductionOperations(
        std::shared_ptr<void> owned_context,
        PairwiseOperationResult<PairwiseProductionRun> (*owned_prepare_run)(
            void*, const AdmittedPairwiseProductionInputs&) noexcept) noexcept
        : context{std::move(owned_context)}, prepare_run{owned_prepare_run} {}
    PairwiseProductionOperations(PairwiseProductionOperations&&) noexcept = default;
    PairwiseProductionOperations(const PairwiseProductionOperations&) = delete;
    auto operator=(PairwiseProductionOperations&&)
        -> PairwiseProductionOperations& = delete;
    auto operator=(const PairwiseProductionOperations&)
        -> PairwiseProductionOperations& = delete;

    std::shared_ptr<void> context;
    PairwiseOperationResult<PairwiseProductionRun> (*prepare_run)(
        void*, const AdmittedPairwiseProductionInputs&) noexcept{};
};

class PairwiseRunnerExecutionBackend final {
  public:
    explicit PairwiseRunnerExecutionBackend(
        PairwiseProductionOperations operations) noexcept
        : operations_{std::move(operations)} {}
    PairwiseRunnerExecutionBackend(PairwiseRunnerExecutionBackend&&) noexcept = default;
    PairwiseRunnerExecutionBackend(const PairwiseRunnerExecutionBackend&) = delete;
    auto operator=(PairwiseRunnerExecutionBackend&&)
        -> PairwiseRunnerExecutionBackend& = delete;
    auto operator=(const PairwiseRunnerExecutionBackend&)
        -> PairwiseRunnerExecutionBackend& = delete;

    [[nodiscard]] auto execute(
        const AdmittedPairwiseProductionInputs& inputs) const noexcept
        -> std::expected<FinalizedPairwiseRunEvidence, core::Error>;

  private:
    PairwiseProductionOperations operations_;
};

// Constructs the selected one-shot production backend from native typed
// dependencies. No legacy host-operation table participates in this route.
[[nodiscard]] auto make_pairwise_production_backend() noexcept
    -> PairwiseOperationResult<PairwiseRunnerExecutionBackend>;

} // namespace epycbench::runtime
