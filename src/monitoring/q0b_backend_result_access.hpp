#pragma once

#include "epycbench/monitoring/q0b_backend_result.hpp"
#include <nlohmann/json.hpp>

namespace epycbench::monitoring::qualification::detail {
[[nodiscard]] auto build_q0b_characterization_payload_with_manifest(
    std::string_view qualification_id, std::string_view collected_at,
    const MonitoringQ0bProductionInputsAuthority& authority,
    const MonitoringQ0bBaseEnvironmentProjection& base,
    const Q0bC0Calibrations& calibrations, nlohmann::json build) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;
}
