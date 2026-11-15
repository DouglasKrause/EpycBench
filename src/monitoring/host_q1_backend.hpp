#pragma once

#include "epycbench/core/error.hpp"
#include "monitoring/host_q1_page_fault_collector.hpp"
#include "epycbench/scenario/structural_validation.hpp"

#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace epycbench::runtime { class ControllerProcessProductionAuthority; }
namespace epycbench::monitoring::qualification::detail {

struct MonitoringHostQ1BackendBytes final { std::string canonical_json; };
struct MonitoringHostQ1BackendAuthored final {
    std::vector<scenario::ScenarioDiagnostic> diagnostics;
};
using MonitoringHostQ1BackendResult =
    std::variant<MonitoringHostQ1BackendBytes, MonitoringHostQ1BackendAuthored>;
struct MonitoringHostQ1BackendFailure final {
    core::Error primary;
    std::vector<core::Error> secondary;
    std::optional<MonitoringQ1OracleMismatch> oracle_mismatch{};
};

[[nodiscard]] auto run_monitoring_host_q1_backend(
    const std::filesystem::path&, std::string_view,
    runtime::ControllerProcessProductionAuthority&) noexcept
    -> std::expected<MonitoringHostQ1BackendResult, MonitoringHostQ1BackendFailure>;
[[nodiscard]] auto project_monitoring_host_q1_error(
    const MonitoringHostQ1BackendFailure&) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto project_monitoring_host_q1_authored(
    std::span<const scenario::ScenarioDiagnostic>) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto monitoring_host_q1_usage_failure() noexcept
    -> MonitoringHostQ1BackendFailure;

} // namespace epycbench::monitoring::qualification::detail
