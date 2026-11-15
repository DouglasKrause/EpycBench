#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/measurement/calibration.hpp"
#include "epycbench/monitoring/production_admission_projection.hpp"
#include "epycbench/monitoring/q0b_base_environment_projection.hpp"

#include <expected>
#include <nlohmann/json_fwd.hpp>

namespace epycbench::monitoring::qualification {

// Projects calibration-owned fields only. The driver adds `finalization` after an actual
// successful consuming PairwiseProductionRun::finalize() call.
[[nodiscard]] auto project_q0b_family_calibration(
    const MonitoringProductionAdmissionProjection& admission,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const measurement::SequentialReadBandwidthCalibrationOutcome& outcome) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;

[[nodiscard]] auto project_q0b_family_calibration(
    const MonitoringProductionAdmissionProjection& admission,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const measurement::DependentLoadLatencyCalibrationOutcome& outcome) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;

} // namespace epycbench::monitoring::qualification
