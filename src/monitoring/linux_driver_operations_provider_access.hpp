#pragma once

#include "epycbench/monitoring/linux_driver_operations_provider.hpp"

#include <optional>

namespace epycbench::monitoring {

struct LinuxMonitoringDriverOperationsProvider::Dependencies {
    void* context{};
    std::expected<void, core::Error> (*bind)(void*, core::LinuxCpuId) noexcept{};
    std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*now)(void*) noexcept{};
    std::expected<MonitoringSchedulerWaitResult, core::Error> (*wait)(
        void*, platform_linux::RawMonotonicTimestamp) noexcept{};
    void (*wake)(void*) noexcept{};
    PageFaultSource (*page)(void*, core::WorkerId, platform_linux::LinuxThreadId) noexcept{};
    std::expected<ContextSwitchEventSource, core::Error> (*context_switch)(
        void*, core::WorkerId, platform_linux::LinuxThreadId) noexcept{};
    std::expected<std::optional<TemperatureSource>, core::Error> (*temperature)(void*) noexcept{};
    std::expected<std::optional<FrequencySource>, core::Error> (*frequency)(
        void*, core::LinuxCpuId) noexcept{};
};

namespace detail {

enum class LinuxProviderReadBoundary { none, page, context, temperature, frequency };
enum class LinuxProviderExceptionKind { standard, unknown };

class LinuxMonitoringDriverOperationsProviderAccess {
  public:
    using Dependencies = LinuxMonitoringDriverOperationsProvider::Dependencies;
    [[nodiscard]] static auto create(Dependencies)
        -> LinuxMonitoringDriverOperationsProvider;
    static void inject_read_exception(LinuxProviderReadBoundary,
                                      LinuxProviderExceptionKind) noexcept;
    static void clear_read_exception() noexcept;
};

} // namespace detail

} // namespace epycbench::monitoring
