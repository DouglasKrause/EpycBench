#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/measurement/calibration.hpp"
#include "epycbench/monitoring/production_admission_projection.hpp"
#include "epycbench/monitoring/q0b_base_environment_projection.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <vector>

namespace epycbench::monitoring::qualification {

namespace detail { class detail_HostQ0bC0CoordinatorAccess; }

struct Q0bC0DriverFailure {
    core::Error primary;
    std::vector<core::Error> secondary;
};

struct Q0bC0FamilyInput {
    const runtime::CalibrationOnlyProductionAdmission& inputs;
    const MonitoringProductionAdmissionProjection& admission;
    const MonitoringQ0bBaseEnvironmentProjection& base;
    measurement::CalibrationDurationPolicy calibration_policy;
};

class Q0bC0Calibrations {
  public:
    [[nodiscard]] auto families() const noexcept -> const nlohmann::ordered_json& {
        return families_;
    }
    [[nodiscard]] auto control_monitor_assignment() const noexcept
        -> const runtime::ServiceThreadAssignment& { return assignment_; }
  private:
    Q0bC0Calibrations(nlohmann::ordered_json families,
                      runtime::ServiceThreadAssignment assignment)
        : families_(std::move(families)), assignment_{std::move(assignment)} {}
    nlohmann::ordered_json families_;
    runtime::ServiceThreadAssignment assignment_;
    friend auto run_q0b_c0_calibrations(
        const runtime::CalibrationOnlyProductionOperations&, const Q0bC0FamilyInput&,
        const Q0bC0FamilyInput&) noexcept
        -> std::expected<Q0bC0Calibrations, Q0bC0DriverFailure>;
    friend class detail::detail_HostQ0bC0CoordinatorAccess;
};

[[nodiscard]] auto run_q0b_c0_calibrations(
    const runtime::CalibrationOnlyProductionOperations& operations,
    const Q0bC0FamilyInput& bandwidth, const Q0bC0FamilyInput& latency) noexcept
    -> std::expected<Q0bC0Calibrations, Q0bC0DriverFailure>;

} // namespace epycbench::monitoring::qualification
