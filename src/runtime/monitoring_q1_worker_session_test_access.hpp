#pragma once

#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"

namespace epycbench::runtime::detail {

struct MonitoringQ1WorkerSessionTestAccess {
    enum class ProjectionFault : std::uint8_t {
        none, bad_alloc, standard_exception, unknown_exception
    };
    [[nodiscard]] static auto create(const AdmittedPlacementRequest&, RunnerCancellation&,
                                     WorkerTeamDependencies)
        -> std::expected<MonitoringQ1WorkerSession, core::Error>;
    static void inject_stop_failure(MonitoringQ1WorkerSession&, std::uint32_t) noexcept;
    static void inject_force_failure(MonitoringQ1WorkerSession&, std::uint8_t, std::uint32_t,
                                     ForcedTeamStopTestFault) noexcept;
    static void set_projection_fault(ProjectionFault) noexcept;
    static void set_force_capacity(MonitoringQ1WorkerSession&, std::size_t) noexcept;
    static void inject_inactive_stop_failure(MonitoringQ1WorkerSession&) noexcept;
    static void inject_eligibility_failure(MonitoringQ1WorkerSession&,
                                           StopEligibilityTestFault) noexcept;
    static void inject_stop_exception(MonitoringQ1WorkerSession&,
                                      MonitoringQ1StopExceptionTestFault) noexcept;
};

} // namespace epycbench::runtime::detail
