#pragma once
#include "cli/monitoring_q1_command.hpp"
namespace epycbench::cli::detail {
using MonitoringQ1Dispatch = std::expected<monitoring::qualification::detail::MonitoringHostQ1BackendResult,
    monitoring::qualification::detail::MonitoringHostQ1BackendFailure> (*)(
        const std::filesystem::path&, std::string_view,
        runtime::ControllerProcessProductionAuthority&) noexcept;
class MonitoringQ1CommandTestAccess final {
  public:
    static void set_dispatch(MonitoringQ1Dispatch) noexcept;
    static void reset() noexcept;
};
}
