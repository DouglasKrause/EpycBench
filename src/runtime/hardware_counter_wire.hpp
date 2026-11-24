#pragma once

#include "epycbench/monitoring/hardware_counter_evidence.hpp"

#include <nlohmann/json_fwd.hpp>

namespace epycbench::runtime::hardware_counter_wire {

[[nodiscard]] auto project(const monitoring::WorkerHardwareCounterEvidence&) -> nlohmann::json;

[[nodiscard]] auto decode(const nlohmann::json&, core::WorkerId expected_worker,
                          platform_linux::RawMonotonicTimestamp enclosing_start,
                          platform_linux::RawMonotonicTimestamp enclosing_end)
    -> std::expected<monitoring::WorkerHardwareCounterEvidence, core::Error>;

} // namespace epycbench::runtime::hardware_counter_wire
