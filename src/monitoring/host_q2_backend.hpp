#pragma once
#include "epycbench/core/error.hpp"
#include "monitoring/host_q2_context_switch_collector.hpp"
#include "epycbench/scenario/structural_validation.hpp"
#include <array>
#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <variant>
#include <vector>
namespace epycbench::runtime { class ControllerProcessProductionAuthority; }
namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ2CollectionStage : std::uint8_t;
enum class MonitoringQ2AuthorityStage : std::uint8_t;
struct MonitoringHostQ2BackendBytes final { std::string canonical_json; };
struct MonitoringHostQ2BackendAuthored final { std::vector<scenario::ScenarioDiagnostic> diagnostics; };
using MonitoringHostQ2BackendResult = std::variant<MonitoringHostQ2BackendBytes, MonitoringHostQ2BackendAuthored>;
enum class MonitoringHostQ2BackendPhase : std::uint8_t { repository,collection_plan,predecessor,
 build_attestation,permission,session,worker_admission,source_preparation,voluntary_event,
 preemption_event,drain,association,clock,completeness,finalization,evidence_construction,usage };
struct MonitoringHostQ2BackendFailure final {
 core::Error primary; std::array<std::optional<core::Error>,63> secondary{};
 std::size_t secondary_count{}; bool cleanup_overflowed{};
 MonitoringHostQ2BackendPhase phase{};
 std::array<char,128> underlying_code{}; std::size_t underlying_code_size{};
 std::array<char,32> repository_classification{}; std::size_t repository_classification_size{};
 std::optional<MonitoringQ2RequiredEventDiagnostic> required_event{};
};
auto run_monitoring_host_q2_backend(const std::filesystem::path&, std::string_view,
    runtime::ControllerProcessProductionAuthority&) noexcept
    -> std::expected<MonitoringHostQ2BackendResult, MonitoringHostQ2BackendFailure>;
auto project_monitoring_host_q2_authored(std::span<const scenario::ScenarioDiagnostic>) noexcept
    -> std::expected<nlohmann::ordered_json, core::Error>;
[[nodiscard]] auto validate_monitoring_host_q2_backend_success(std::string_view) noexcept -> bool;
auto monitoring_host_q2_usage_failure() noexcept -> MonitoringHostQ2BackendFailure;
[[nodiscard]] auto monitoring_q2_backend_phase(MonitoringQ2CollectionStage) noexcept
    -> MonitoringHostQ2BackendPhase;
[[nodiscard]] auto monitoring_q2_backend_phase(MonitoringQ2AuthorityStage) noexcept
    -> MonitoringHostQ2BackendPhase;
}
