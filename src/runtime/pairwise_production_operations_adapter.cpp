#include "epycbench/runtime/pairwise_production_operations_adapter.hpp"
#include "runtime/pairwise_production_operations_support.hpp"

#include <concepts>
#include <exception>
#include <type_traits>
#include <utility>

namespace epycbench::runtime {
namespace {

auto calibration_failure(detail::LegacyProductionOperationFailure failure) noexcept
    -> CalibrationOnlyOperationFailure {
    if (failure.active_block_id) std::terminate();
    return {std::move(failure.primary), std::move(failure.secondary)};
}

template <typename T>
auto calibration_result(detail::LegacyProductionOperationResult<T> result) noexcept
    -> CalibrationOnlyOperationResult<T> {
    if (!result) {
        return std::unexpected{calibration_failure(std::move(result.error()))};
    }
    return std::move(*result);
}

auto calibration_result(detail::LegacyProductionOperationResult<void> result) noexcept
    -> CalibrationOnlyOperationResult<void> {
    if (!result) {
        return std::unexpected{calibration_failure(std::move(result.error()))};
    }
    return {};
}

class CalibrationRunAdapter final
    : public CalibrationOnlyProductionRun::Implementation {
  public:
    void install(std::unique_ptr<detail::CalibrationProductionKernel> kernel) noexcept {
        if (kernel_ || !kernel) std::terminate();
        kernel_ = std::move(kernel);
    }

    auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> CalibrationOnlyOperationResult<
            measurement::SequentialReadBandwidthCalibrationOutcome> override {
        return calibration_result(kernel_->calibrate_bandwidth(policy));
    }
    auto calibrate_latency(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> CalibrationOnlyOperationResult<
            measurement::DependentLoadLatencyCalibrationOutcome> override {
        return calibration_result(kernel_->calibrate_latency(policy));
    }
    auto finalize() noexcept -> CalibrationOnlyOperationResult<void> override {
        return calibration_result(kernel_->finalize_calibration());
    }

  private:
    std::unique_ptr<detail::CalibrationProductionKernel> kernel_;
};

static_assert(std::is_final_v<CalibrationRunAdapter>);
static_assert(std::derived_from<CalibrationRunAdapter,
                                CalibrationOnlyProductionRun::Implementation>);

} // namespace

PairwiseProductionOperationsAdapter::PairwiseProductionOperationsAdapter(
    PairwiseProductionHostOperations host)
    : state_{detail::make_production_adapter_state(std::move(host))} {}

auto PairwiseProductionOperationsAdapter::calibration_operations() const noexcept
    -> std::expected<CalibrationOnlyProductionOperations, core::Error> {
    return CalibrationOnlyProductionOperations::create(state_, prepare_calibration_run);
}

auto PairwiseProductionOperationsAdapter::prepare_calibration_run(
    void* opaque, const CalibrationOnlyProductionAdmission& inputs) noexcept
    -> CalibrationOnlyOperationResult<CalibrationOnlyProductionRun> {
    try {
        auto adapter = std::make_unique<CalibrationRunAdapter>();
        auto prepared = detail::prepare_calibration_production_kernel(
            *static_cast<State*>(opaque), inputs);
        if (!prepared)
            return std::unexpected{calibration_failure(std::move(prepared.error()))};
        adapter->install(std::move(*prepared));
        return CalibrationOnlyProductionRun{std::move(adapter)};
    } catch (const std::exception&) {
        return std::unexpected{CalibrationOnlyOperationFailure{
            detail::production_adapter_boundary_error("contained_exception"), {}}};
    } catch (...) {
        return std::unexpected{CalibrationOnlyOperationFailure{
            detail::production_adapter_boundary_error("contained_unknown_failure"), {}}};
    }
}

} // namespace epycbench::runtime
