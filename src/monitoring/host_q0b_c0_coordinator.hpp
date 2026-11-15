#pragma once

#include "monitoring/host_qualification_composition.hpp"
#include "monitoring/host_q0b_v2_test_access.hpp"
#include "epycbench/monitoring/host_q0b_v2_projection.hpp"

namespace epycbench::monitoring::qualification::detail {

struct HostQ0bStageAdmission final {
    void* context{};
    auto (*admit)(void*, const RunnerContextAcquisitionSession&,
        const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&,
        HostQ0bFamily) -> std::expected<void, Q0bC0DriverFailure>{};
};

class detail_HostQ0bC0CoordinatorAccess final {
  public:
    static auto make(nlohmann::ordered_json families,
                     runtime::ServiceThreadAssignment assignment) -> Q0bC0Calibrations {
        return Q0bC0Calibrations{std::move(families), std::move(assignment)};
    }
};

[[nodiscard]] auto coordinate_host_q0b_c0_calibrations(
    std::string_view, std::string_view, const HostEnvironmentAuthority&,
    const RunnerContextAcquisitionSession&,
    const runtime::CalibrationOnlyProductionOperations&,
    const HostQualificationCalibrationInputs&, const MonitoringHostQualificationPlan&,
    const MonitoringHostAdmittedProductionFacts&, const MonitoringHostAdmittedProductionFacts&,
    const MonitoringHostQ0bBaseEnvironment&, HostQ0bStageAdmission = {}) noexcept
    -> std::expected<MonitoringHostQ0bBackendResult, Q0bC0DriverFailure>;

} // namespace epycbench::monitoring::qualification::detail
