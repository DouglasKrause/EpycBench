#pragma once

#include "epycbench/monitoring/host_q0b_v2_projection.hpp"
#include "epycbench/monitoring/q0b_backend_driver.hpp"
#include "epycbench/monitoring/host_runner_context_session.hpp"
#include "monitoring/host_q0b_c0_coordinator.hpp"

#include <filesystem>
#include <memory>

namespace epycbench::runtime { class ControllerProcessProductionAuthority; }

namespace epycbench::monitoring::qualification::detail {

class HostQualificationCalibrationInputs;

struct HostQ0bBackendOperations final {
  std::shared_ptr<void> owner;
  runtime::CalibrationOnlyProductionOperations operations;
};
struct HostQ0bBackendDependencies final {
  void* context{};
  auto (*load)(void*,const std::filesystem::path&)
    ->std::expected<MonitoringHostInputTemplateAuthority,core::Error>{};
  auto (*acquire)(void*,MonitoringHostInputTemplateAuthority)
    ->std::expected<RunnerContextAcquisitionSession,core::Error>{};
  auto (*make_operations)(void*)
    ->std::expected<HostQ0bBackendOperations,core::Error>{};
  auto (*clock)(void*)->std::expected<std::string,core::Error>{};
  // Optional test boundary after the four governed IDs are allocated and
  // before any facts, operations, clock, or lifecycle work is reached.
  auto (*after_composition)(void*)->std::expected<void,core::Error>{};
  // Optional source-private fault boundary used only to prove every owning
  // sequence stage's transport and suppression behavior.
  auto (*before_stage)(void*,std::string_view)->std::expected<void,core::Error>{};
  // Optional pre-work admission/retention on the exact original session and inputs.
  auto (*prepare_baseline)(void*, const RunnerContextAcquisitionSession&,
      const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&)
      ->std::expected<void,core::Error>{};
  // Optional governed pre-work boundary, after each matched revalidation and
  // immediately before the corresponding actual calibration preparation.
  HostQ0bStageAdmission stage_admission{};
};

[[nodiscard]] auto run_host_q0b_backend_driver(
    const std::filesystem::path&, std::string_view,
    runtime::ControllerProcessProductionAuthority&) noexcept
    -> std::expected<MonitoringHostQ0bBackendResult,Q0bBackendFailure>;
[[nodiscard]] auto run_host_q0b_backend_driver_with_dependencies(
    const std::filesystem::path&,std::string_view,
    runtime::ControllerProcessProductionAuthority&,const HostQ0bBackendDependencies&) noexcept
    ->std::expected<MonitoringHostQ0bBackendResult,Q0bBackendFailure>;
[[nodiscard]] auto apply_host_q0b_failure_stage_for_test(
    Q0bBackendFailure,std::string_view)->Q0bBackendFailure;
// Inactive owners reuse exactly the default dependencies and failure projector.
[[nodiscard]] auto production_host_q0b_backend_dependencies() noexcept
    -> HostQ0bBackendDependencies;
[[nodiscard]] auto project_host_q0b_failure_stage(
    Q0bBackendFailure, std::string_view) -> Q0bBackendFailure;

} // namespace epycbench::monitoring::qualification::detail
