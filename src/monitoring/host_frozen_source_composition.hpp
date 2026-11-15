#pragma once
#include "epycbench/monitoring/host_environment.hpp"
#include "monitoring/host_qualification_composition.hpp"
#include <optional>
#include <utility>
namespace epycbench::monitoring::qualification::detail {
enum class HostFrozenCompositionStage : unsigned char {
    environment,
    plan,
    registration,
    calibration
};
// Original Session precedes this owner and outlives every admitted context borrow.
class HostFrozenSourceComposition final {
  public:
    HostFrozenSourceComposition(HostFrozenSourceComposition&&) noexcept = default;
    HostFrozenSourceComposition(const HostFrozenSourceComposition&) = delete;
    auto operator=(HostFrozenSourceComposition&&) -> HostFrozenSourceComposition& = delete;
    static auto prepare(const RunnerContextAcquisitionSession&, runtime::ProductionRequestIdSource&,
                        HostFrozenCompositionStage&, std::optional<core::Error>* = nullptr)
        -> std::expected<HostFrozenSourceComposition, core::Error>;
    auto environment() const noexcept -> const HostEnvironmentAuthority& { return environment_; }
    auto plan() const noexcept -> const MonitoringHostQualificationPlan& { return plan_; }
    auto calibration() const noexcept -> const HostQualificationCalibrationInputs& {
        return calibration_;
    }

  private:
    HostFrozenSourceComposition(HostEnvironmentAuthority environment,
                                MonitoringHostQualificationPlan plan,
                                HostQualificationCalibrationInputs calibration) noexcept
        : environment_{std::move(environment)}, plan_{std::move(plan)},
          calibration_{std::move(calibration)} {}
    HostEnvironmentAuthority environment_;
    MonitoringHostQualificationPlan plan_;
    HostQualificationCalibrationInputs calibration_;
};
} // namespace epycbench::monitoring::qualification::detail
