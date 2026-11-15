#pragma once

#include "epycbench/runtime/pairwise_production_operations_adapter.hpp"

#include <memory>

namespace epycbench::runtime::detail {

struct LegacyProductionOperationFailure final {
    core::Error primary;
    std::vector<core::Error> secondary;
    std::optional<kernel::DependentLoadLatencyBlockId> active_block_id{};
};

template <class T>
using LegacyProductionOperationResult =
    std::expected<T, LegacyProductionOperationFailure>;

struct LegacyPairwiseFinalizationOutcome final {
    PairwiseFinalizationEvidence evidence;
    std::vector<LocalFinalizationErrorAssociation> associations;
    std::vector<core::Error> errors;
};

auto make_production_adapter_state(PairwiseProductionHostOperations)
    -> std::shared_ptr<PairwiseProductionOperationsAdapter::State>;
auto production_adapter_boundary_error(const char* classification) -> core::Error;

// Source-private ownership kernels deliberately implement neither public run
// interface.  The public-interface adapters live in the thin adapter TU.
class CalibrationProductionKernel {
  public:
    virtual ~CalibrationProductionKernel() noexcept = default;
    virtual auto calibrate_bandwidth(
        const measurement::CalibrationDurationPolicy&) noexcept
        -> LegacyProductionOperationResult<
            measurement::SequentialReadBandwidthCalibrationOutcome> = 0;
    virtual auto calibrate_latency(
        const measurement::CalibrationDurationPolicy&) noexcept
        -> LegacyProductionOperationResult<
            measurement::DependentLoadLatencyCalibrationOutcome> = 0;
    virtual auto finalize_calibration() noexcept
        -> LegacyProductionOperationResult<void> = 0;
};

auto prepare_calibration_production_kernel(
    PairwiseProductionOperationsAdapter::State&,
    const CalibrationOnlyProductionAdmission&) noexcept
    -> LegacyProductionOperationResult<std::unique_ptr<CalibrationProductionKernel>>;

} // namespace epycbench::runtime::detail
