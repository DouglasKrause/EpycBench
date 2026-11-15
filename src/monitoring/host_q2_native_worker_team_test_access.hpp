#pragma once
#include "monitoring/host_q2_native_worker_team.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2NativeWorkerTeamTestAccess final {
  public:
    static auto terminal_without_park_offer(const MonitoringQ2NativeWorkerTeam&,
                                             std::size_t) noexcept -> bool;
    static void request_command_stop(MonitoringQ2NativeWorkerTeam&, std::size_t) noexcept;
    static void request_team_stop(MonitoringQ2NativeWorkerTeam&) noexcept;
};
} // namespace epycbench::monitoring::qualification::detail
