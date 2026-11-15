#pragma once
#include "runtime/ipc/placement_request_construction_access.hpp"

namespace epycbench::runtime::detail {
// Ordinary supplied-value projection. No source acquisition or Monitoring dependency.
[[nodiscard]] auto project_scenario_monitoring_inputs(const nlohmann::ordered_json&)
    -> std::expected<std::optional<ipc::detail::MonitoringValues>, core::Error>;
} // namespace epycbench::runtime::detail
