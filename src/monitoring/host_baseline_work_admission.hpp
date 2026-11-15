#pragma once

#include "monitoring/host_baseline_preparation.hpp"
#include "platform_linux/host_work_admission.hpp"

namespace epycbench::monitoring::qualification::detail {

struct HostBaselineWorkFailure final {
    std::optional<HostBaselinePreparationFailure> preparation;
    std::optional<platform_linux::host_work::Failure> admission;
};
class HostBaselineWorkAdmission final {
  public:
    HostBaselineWorkAdmission(const HostBaselineWorkAdmission&) = delete;
    auto operator=(const HostBaselineWorkAdmission&) -> HostBaselineWorkAdmission& = delete;
    HostBaselineWorkAdmission(HostBaselineWorkAdmission&&) noexcept = default;
    auto operator=(HostBaselineWorkAdmission&&) noexcept -> HostBaselineWorkAdmission& = default;
    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto headroom() const noexcept
        -> std::optional<platform_linux::host_work::HeadroomReceipt>;
    [[nodiscard]] auto limit_demand() const noexcept
        -> std::optional<platform_linux::host_work::LimitDemand>;
    [[nodiscard]] auto take_preparation() noexcept
        -> std::expected<HostBaselinePreparation, platform_linux::host_work::Reason>;
  private:
    HostBaselineWorkAdmission(HostBaselinePreparation,
        platform_linux::host_work::HeadroomReceipt, platform_linux::host_work::LimitDemand) noexcept;
    HostBaselinePreparation preparation_;
    platform_linux::host_work::HeadroomReceipt headroom_;
    platform_linux::host_work::LimitDemand demand_;
    friend auto admit_host_baseline_work(const RunnerContextAcquisitionSession&,
        const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&,
        core::ByteCount, std::size_t, const platform_linux::host_work::ReadOperations&) noexcept
        -> std::expected<HostBaselineWorkAdmission, HostBaselineWorkFailure>;
};
// Ordinary factory preparation precedes allocation-sensitive read/limit paths.
// Stack/descriptor demand comes from the eventual actual prepared native owner.
[[nodiscard]] auto admit_host_baseline_work(const RunnerContextAcquisitionSession&,
    const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&,
    core::ByteCount stack_demand, std::size_t descriptor_demand,
    const platform_linux::host_work::ReadOperations&) noexcept
    -> std::expected<HostBaselineWorkAdmission, HostBaselineWorkFailure>;

} // namespace epycbench::monitoring::qualification::detail
