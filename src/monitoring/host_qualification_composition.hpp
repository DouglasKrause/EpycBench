#pragma once

#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/runtime/pairwise_production_composition.hpp"

#include <expected>
#include <optional>

namespace epycbench::monitoring::qualification::detail {

class HostQualificationCalibrationInputs final {
  public:
    HostQualificationCalibrationInputs(const HostQualificationCalibrationInputs&) = delete;
    auto operator=(const HostQualificationCalibrationInputs&)
        -> HostQualificationCalibrationInputs& = delete;
    HostQualificationCalibrationInputs(HostQualificationCalibrationInputs&&) noexcept = default;
    auto operator=(HostQualificationCalibrationInputs&&)
        -> HostQualificationCalibrationInputs& = delete;

    [[nodiscard]] auto bandwidth() const noexcept
        -> const runtime::CalibrationOnlyProductionAdmission& { return bandwidth_; }
    [[nodiscard]] auto latency() const noexcept
        -> const runtime::CalibrationOnlyProductionAdmission& { return latency_; }

  private:
    HostQualificationCalibrationInputs(runtime::CalibrationOnlyProductionAdmission bandwidth,
                                       runtime::CalibrationOnlyProductionAdmission latency) noexcept
        : bandwidth_{std::move(bandwidth)}, latency_{std::move(latency)} {}
    runtime::CalibrationOnlyProductionAdmission bandwidth_;
    runtime::CalibrationOnlyProductionAdmission latency_;
    friend auto compose_host_qualification_calibration_inputs_fallible(
        const MonitoringHostQualificationPlan&, const RunnerContextAcquisitionSession&,
        runtime::PairwiseProductionCompositionContext&, bool, std::optional<core::Error>*)
        -> std::expected<HostQualificationCalibrationInputs, core::Error>;
    friend auto compose_host_qualification_calibration_inputs(
        const MonitoringHostQualificationPlan&, const RunnerContextAcquisitionSession&,
        runtime::PairwiseProductionCompositionContext&) noexcept
        -> std::expected<HostQualificationCalibrationInputs, core::Error>;
};

[[nodiscard]] auto compose_host_qualification_calibration_inputs_fallible(
    const MonitoringHostQualificationPlan&, const RunnerContextAcquisitionSession&,
    runtime::PairwiseProductionCompositionContext&, bool source_route,
    std::optional<core::Error>* = nullptr)
    -> std::expected<HostQualificationCalibrationInputs, core::Error>;

[[nodiscard]] auto compose_host_qualification_calibration_inputs(
    const MonitoringHostQualificationPlan&, const RunnerContextAcquisitionSession&,
    runtime::PairwiseProductionCompositionContext&) noexcept
    -> std::expected<HostQualificationCalibrationInputs, core::Error>;

} // namespace epycbench::monitoring::qualification::detail
