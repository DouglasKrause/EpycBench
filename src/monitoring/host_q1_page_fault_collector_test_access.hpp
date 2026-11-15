#pragma once

#include "monitoring/host_q1_page_fault_collector.hpp"
#include "platform_linux/clock_operations.hpp"
#include "platform_linux/thread_rusage_operations.hpp"
#include "epycbench/platform_linux/task_stat.hpp"
#include "runtime/worker_team_state.hpp"
#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ1CollectorCallbackFault : std::uint8_t {
    none, missing_signal, duplicate_signal, caller_failure, success_bad_token,
    caller_failure_bad_token, request_cancellation
};

enum class MonitoringQ1CollectorControlEvent : std::uint8_t {
    preparation, sandwich
};

struct MonitoringQ1PageFaultCollectorDependencies final {
    runtime::detail::WorkerTeamDependencies workers;
    platform_linux::detail::ClockOperations clock;
    platform_linux::detail::ThreadRusageOperations oracle;
    void* source_context{};
    std::expected<platform_linux::LinuxTaskPageFaultCounters, core::Error> (*source_read)(
        void*, platform_linux::LinuxThreadId) noexcept{};
    void* session_context{};
    void (*session_ready)(void*, runtime::detail::MonitoringQ1WorkerSession&) noexcept{};
    MonitoringQ1CollectorCallbackFault callback_fault{};
    runtime::RunnerCancellation* cancellation_to_request{};
    void* control_event_context{};
    void (*control_event)(void*, core::WorkerId,
                          MonitoringQ1CollectorControlEvent) noexcept{};
};

class MonitoringQ1PageFaultCollectorAccess final {
  public:
    [[nodiscard]] static auto make_failure(
        MonitoringQ1PageFaultCollectionStage stage, core::Error primary,
        std::vector<core::Error> secondary,
        std::optional<MonitoringQ1OracleMismatch> witness = std::nullopt)
        -> MonitoringQ1PageFaultCollectionFailure {
        return {stage, std::move(primary), std::move(secondary), witness};
    }
    [[nodiscard]] static auto collect(
        MonitoringQ1CollectorPlan&&, runtime::RunnerCancellation&,
        MonitoringQ1PageFaultCollectorDependencies) noexcept
        -> std::expected<MonitoringQ1PageFaultCollection,
                         MonitoringQ1PageFaultCollectionFailure>;
    [[nodiscard]] static auto make_collection(
        MonitoringQ1CollectorPlan plan,
        std::vector<MonitoringQ1PageFaultObservation> observations,
        std::vector<MonitoringQ1PageFaultSummary> workers,
        MonitoringQ1PageFaultSummary global) -> MonitoringQ1PageFaultCollection {
        return {std::move(plan), std::move(observations), std::move(workers), std::move(global)};
    }
};

} // namespace epycbench::monitoring::qualification::detail
