#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/platform_linux/q4_capacity_control.hpp"
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <array>
#include <variant>
#include "monitoring/host_q4_authority_session.hpp"
#include "monitoring/host_q4_kernel_maximum_authority.hpp"
#include "monitoring/host_q4_source_ready.hpp"
#include "monitoring/host_q4_backend_transport.hpp"

namespace epycbench::runtime { class ControllerProcessProductionAuthority; }
namespace epycbench::monitoring::qualification::detail {

enum class MonitoringHostQ4BackendOutputKind : unsigned char { success, failure, authored };
struct MonitoringHostQ4BackendOutput final {
    MonitoringHostQ4BackendOutputKind kind{};
    std::string canonical_json;
    std::uint64_t byte_limit{};
};
using MonitoringHostQ4BackendStageResult=std::variant<std::monostate,MonitoringQ4AuthorityFailure,
    MonitoringQ4KernelMaximumFailure,MonitoringQ4SourceReadyFailure,MonitoringHostQ4BackendOutput>;
struct MonitoringHostQ4BackendStages final {
    void* context{};
    std::array<std::expected<MonitoringHostQ4BackendStageResult,core::Error>(*)(void*),7> stage{};
};
[[nodiscard]] auto project_monitoring_host_q4_capacity_control_failure(
    const platform_linux::Q4CapacityControlResult&)
    -> MonitoringHostQ4BackendFailure;
[[nodiscard]] auto run_monitoring_host_q4_backend_stages(MonitoringHostQ4BackendStages) noexcept
    -> std::expected<MonitoringHostQ4BackendOutput,core::Error>;
[[nodiscard]] auto project_monitoring_host_q4_adapter_result(
    MonitoringQ4SourceReady&,
    std::expected<std::string,MonitoringHostQ4BackendFailure>) noexcept
    ->std::expected<MonitoringHostQ4BackendOutput,core::Error>;

[[nodiscard]] auto run_monitoring_host_q4_backend(
    const std::filesystem::path&, std::string_view,
    runtime::ControllerProcessProductionAuthority&, int capacity_control_fd) noexcept
    -> std::expected<MonitoringHostQ4BackendOutput, core::Error>;

} // namespace epycbench::monitoring::qualification::detail
