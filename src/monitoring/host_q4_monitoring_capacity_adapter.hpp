#pragma once

#include "monitoring/host_q4_kernel_maximum_authority.hpp"
#include "epycbench/runtime/runner_context.hpp"

namespace epycbench::monitoring::qualification::detail {

[[nodiscard]] auto monitoring_q4_linux_capacity_reason(
    platform_linux::detail::LinuxMonitoringCapacityReason) noexcept
    -> MonitoringQ4CapacityReason;

class MonitoringQ4MonitoringCapacityAdapter final {
  public:
    [[nodiscard]] static auto prepare(const MonitoringQ4KernelMaximumReady&) noexcept
        -> std::expected<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority,
                         MonitoringQ4KernelMaximumFailure>;
    [[nodiscard]] static auto prepare_with(const MonitoringQ4KernelMaximumReady&,
                                           const runtime::RunnerContext&) noexcept
        -> std::expected<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority,
                         MonitoringQ4KernelMaximumFailure>;
    [[nodiscard]] static auto seal(
        platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority&&,
        std::uint32_t) noexcept
        -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
};

} // namespace epycbench::monitoring::qualification::detail
