#pragma once
#include "cli/monitoring_q4_command.hpp"
#include "monitoring/host_q4_backend.hpp"
namespace epycbench::cli::detail {
using MonitoringQ4Dispatch = std::expected<monitoring::qualification::detail::MonitoringHostQ4BackendOutput,
                                           core::Error> (*)(const std::filesystem::path&,
                                               std::string_view,
                                               runtime::ControllerProcessProductionAuthority&,int);
class MonitoringQ4CommandTestAccess final {
  public:
    static void set(MonitoringQ4Dispatch) noexcept;
    static void reset() noexcept;
};
}
