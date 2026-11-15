#pragma once

#include "epycbench/runtime/production_control_monitor.hpp"
#include "runtime/service_platform_operations.hpp"

namespace epycbench::runtime::detail {

struct ProductionControlMonitorAccess {
    [[nodiscard]] static auto startup(const ServiceThreadAssignment& assignment,
                                      const ServicePlatformOperations& platform) noexcept
        -> std::expected<ServiceThreadReadyEvidence, core::Error>;
    [[nodiscard]] static auto monitor_once(ipc::RunnerControlState& control_state,
                                           RunnerControlReadCapability& control,
                                           RunnerCancellation& cancellation,
                                           std::optional<ipc::CancelReason>& recorded_reason,
                                           bool& eof) noexcept -> std::expected<bool, core::Error>;
    [[nodiscard]] static auto create(const AdmittedPlacementRequest& admitted,
                                     std::unique_ptr<ipc::RunnerControlState> control_state,
                                     RunnerControlReadCapability& control,
                                     RunnerCancellation& cancellation,
                                     ServicePlatformOperations platform) noexcept
        -> std::expected<RunnerControlMonitor, core::Error>;
};

} // namespace epycbench::runtime::detail
