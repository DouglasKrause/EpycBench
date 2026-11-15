#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/monitoring/q0b_c0_driver.hpp"
#include "epycbench/monitoring/q0b_production_inputs.hpp"
#include "epycbench/scenario/structural_validation.hpp"

#include <expected>
#include <nlohmann/json_fwd.hpp>
#include <span>

namespace epycbench::monitoring::qualification {

inline constexpr auto q0b_backend_error_schema = "epycbench.q0b-backend-error/v1";
inline constexpr auto q0b_backend_authored_diagnostics_schema =
    "epycbench.q0b-backend-authored-diagnostics/v1";
inline constexpr auto monitoring_work_characterization_schema =
    "epycbench.monitoring-work-characterization/v1";

[[nodiscard]] auto project_q0b_backend_error(const core::Error& failure) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto project_q0b_backend_error(const Q0bC0DriverFailure& failure) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto project_q0b_backend_authored_diagnostics(
    std::span<const scenario::ScenarioDiagnostic> diagnostics) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;

[[nodiscard]] auto build_q0b_characterization_payload(
    std::string_view qualification_id, std::string_view collected_at,
    const MonitoringQ0bProductionInputsAuthority& authority,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const Q0bC0Calibrations& calibrations) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;

} // namespace epycbench::monitoring::qualification
