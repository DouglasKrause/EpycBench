#pragma once
#include "cli/monitoring_q2_command.hpp"
namespace epycbench::cli::detail {
using MonitoringQ2Dispatch = std::expected<monitoring::qualification::detail::MonitoringHostQ2BackendResult,
    monitoring::qualification::detail::MonitoringHostQ2BackendFailure> (*)(
        const std::filesystem::path&, std::string_view,
        runtime::ControllerProcessProductionAuthority&) noexcept;
using MonitoringQ2BeforeFailureSerialization = void (*)() noexcept;
class MonitoringQ2CommandTestAccess final { public:
 static void set_dispatch(MonitoringQ2Dispatch) noexcept;
 static void set_before_failure_serialization(MonitoringQ2BeforeFailureSerialization) noexcept;
 static void set_after_failure_serialization(MonitoringQ2BeforeFailureSerialization) noexcept;
 static void reset() noexcept;
};
}
