#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/runtime/runner_context.hpp"
#include "epycbench/scenario/structural_validation.hpp"

#include <expected>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string_view>
#include <variant>
#include <vector>

namespace epycbench::runtime { class ControllerProcessProductionAuthority; }

namespace epycbench::monitoring::qualification {

struct Q0bBackendFailure { core::Error primary; std::vector<core::Error> secondary; };
struct Q0bBackendAuthoredDiagnostics { std::vector<scenario::ScenarioDiagnostic> diagnostics; };
using Q0bBackendResult = std::variant<nlohmann::ordered_json, Q0bBackendAuthoredDiagnostics>;

struct Q0bBackendDependencies {
    runtime::RunnerContextProvider runner_context;
    runtime::CalibrationOnlyProductionOperations operations;
};

[[nodiscard]] auto run_q0b_backend_driver(
    const std::filesystem::path& repository_root, std::string_view qualification_id,
    std::string_view collected_at, runtime::ControllerProcessProductionAuthority& authority,
    const Q0bBackendDependencies& dependencies) noexcept
    -> std::expected<Q0bBackendResult, Q0bBackendFailure>;

} // namespace epycbench::monitoring::qualification
