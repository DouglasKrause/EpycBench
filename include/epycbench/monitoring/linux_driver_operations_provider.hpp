#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"

#include <memory>

namespace epycbench::monitoring {

namespace detail { class LinuxMonitoringDriverOperationsProviderAccess; }

class LinuxMonitoringDriverOperationsProvider {
  public:
    struct Context; // Opaque provider-owned callback state.
    struct Dependencies; // Defined only by the source-tree-private test seam.
    LinuxMonitoringDriverOperationsProvider(
        LinuxMonitoringDriverOperationsProvider&&) noexcept;
    auto operator=(LinuxMonitoringDriverOperationsProvider&&)
        -> LinuxMonitoringDriverOperationsProvider& = delete;
    LinuxMonitoringDriverOperationsProvider(
        const LinuxMonitoringDriverOperationsProvider&) = delete;
    auto operator=(const LinuxMonitoringDriverOperationsProvider&)
        -> LinuxMonitoringDriverOperationsProvider& = delete;
    ~LinuxMonitoringDriverOperationsProvider() noexcept;

    [[nodiscard]] auto operations() noexcept -> MonitoringDriverOperations;

  private:
    explicit LinuxMonitoringDriverOperationsProvider(std::unique_ptr<Context>) noexcept;
    std::unique_ptr<Context> context_;
    friend class detail::LinuxMonitoringDriverOperationsProviderAccess;
    friend auto make_linux_monitoring_driver_operations_provider()
        -> LinuxMonitoringDriverOperationsProvider;
};

// Construction is structural: source discovery/preparation and host access begin only when
// prepare_sources is invoked through operations().
[[nodiscard]] auto make_linux_monitoring_driver_operations_provider()
    -> LinuxMonitoringDriverOperationsProvider;

} // namespace epycbench::monitoring
