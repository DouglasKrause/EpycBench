#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"
#include "epycbench/runtime/ipc/payloads.hpp"

namespace epycbench::runtime::detail {

struct ProductionMonitoringPolicyView {
    std::span<const ipc::PlacementMonitoringChannel> required, optional;
    bool before, after, during;
    std::optional<core::Duration> interval;
    ipc::PlacementOptionalUnavailableSourcePolicy unavailable;
};
[[nodiscard]] auto project_production_monitoring_policy(ProductionMonitoringPolicyView,
                                                        std::optional<core::Error>* = nullptr)
    -> std::expected<monitoring::MonitoringExecutionPolicy, core::Error>;

[[nodiscard]] auto project_production_monitoring_policy(const ipc::PlacementRequest& request,
                                                        std::optional<core::Error>* = nullptr)
    -> std::expected<monitoring::MonitoringExecutionPolicy, core::Error>;
[[nodiscard]] auto wrap_production_monitoring_policy(
    std::expected<monitoring::MonitoringExecutionPolicy, core::Error> policy,
    std::optional<core::Error>* = nullptr)
    -> std::expected<monitoring::MonitoringExecutionPolicy, core::Error>;
[[nodiscard]] auto production_monitoring_needs_service(
    const monitoring::MonitoringExecutionPolicy& policy) noexcept -> bool;

} // namespace epycbench::runtime::detail
