#pragma once

#include "cli/monitoring_q3_command.hpp"
#include "monitoring/host_q3_backend.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace epycbench::cli::detail {
using MonitoringQ3Dispatch =
    std::expected<monitoring::qualification::detail::MonitoringHostQ3BackendResult,
                  monitoring::qualification::detail::MonitoringHostQ3BackendFailure> (*)(
        const std::filesystem::path&, std::string_view,
        runtime::ControllerProcessProductionAuthority&);
using MonitoringQ3Validate = bool (*)(std::string_view);
using MonitoringQ3SerializeFailure = std::expected<std::string, core::Error> (*)(
    const monitoring::qualification::detail::MonitoringHostQ3BackendFailure&);
using MonitoringQ3SerializeAuthored = std::expected<std::string, core::Error> (*)(
    const monitoring::qualification::detail::MonitoringHostQ3BackendAuthored&);
class MonitoringQ3CommandTestAccess final {
  public:
    static void set(MonitoringQ3Dispatch, MonitoringQ3Validate, MonitoringQ3SerializeFailure,
                    MonitoringQ3SerializeAuthored) noexcept;
    static void reset() noexcept;
};
} // namespace epycbench::cli::detail
