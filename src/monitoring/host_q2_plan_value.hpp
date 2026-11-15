#pragma once

#include "epycbench/core/error.hpp"

#include <cstdint>
#include <expected>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ2PlanValue final {
    static constexpr std::string_view schema =
        "epycbench.monitoring-q2-context-switch-collection-plan/v1";
    static constexpr std::string_view path =
        "config/monitoring-qualification/host-v2-q2-context-switch-collection-plan.json";

    std::uint64_t voluntary_trials_per_worker{};
    std::uint64_t preemption_trials_per_worker{};
    std::uint64_t periodic_drain_after_trials{};
    std::uint64_t ring_data_pages{};
    std::uint64_t per_trial_deadline_ms{};
    std::uint64_t execution_deadline_seconds{};
    std::uint64_t safety_iteration_ceiling{};
    std::uint64_t fixed_reserve_seconds{};
    std::uint64_t maximum_worker_count{};
    std::uint64_t maximum_total_trials{};
    std::uint64_t total_decoded_records{};
    std::uint64_t records_per_drain{};
    std::uint64_t drains{};
    std::uint64_t raw_in_memory_bytes{};
    std::uint64_t canonical_evidence_bytes{};
    std::uint64_t backend_stdout_bytes{};
    std::uint64_t backend_stderr_bytes{};
    std::uint64_t diagnostic_count{};
};

[[nodiscard]] auto decode_monitoring_q2_plan(std::string_view)
    -> std::expected<MonitoringQ2PlanValue, core::Error>;

} // namespace epycbench::monitoring::qualification::detail
