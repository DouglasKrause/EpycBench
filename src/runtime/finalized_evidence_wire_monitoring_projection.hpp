#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"

#include <nlohmann/json_fwd.hpp>

namespace epycbench::runtime::wire_v1::detail {

struct ComponentProjectionCount;
[[nodiscard]] auto count_monitoring_projection(const monitoring::MonitoringCaptureResult&)
    -> ComponentProjectionCount;

[[nodiscard]] auto project_monitoring(
    const monitoring::MonitoringCaptureResult& capture) -> nlohmann::json;

} // namespace epycbench::runtime::wire_v1::detail
