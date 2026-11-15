#pragma once

#include "epycbench/monitoring/q0b_production_inputs.hpp"
#include <nlohmann/json.hpp>

namespace epycbench::monitoring::qualification {
enum class Q0bManifestFamily { bandwidth, latency };
struct Q0bManifestEntry { std::string family; std::string scenario_path; core::Sha256Digest scenario_sha256; };
struct Q0bManifest { std::string schema; std::string policy_identity; core::Sha256Digest policy_sha256; std::vector<Q0bManifestEntry> families; };

class detail_MonitoringQ0bProductionInputsAccess {
  public:
    static auto decode_manifest(std::string_view bytes) noexcept -> std::expected<Q0bManifest, core::Error>;
    static auto validate_manifest(const Q0bManifest& manifest) noexcept -> std::expected<void, core::Error>;
    static auto validate_consistency(Q0bManifestFamily family, const scenario::ScenarioKernelBindings& bindings) noexcept -> std::expected<void, core::Error>;
    static auto validate_consistency_for_test(Q0bManifestFamily family,
        const std::vector<scenario::ScenarioKernelBinding>& bindings) noexcept
        -> std::expected<void, core::Error>;
    static auto make_diagnostics(scenario::ScenarioDiagnostic first,
        std::vector<scenario::ScenarioDiagnostic> remaining)
        -> MonitoringQ0bProductionInputsLoadResult;
    static auto process_authenticated_scenario_for_test(Q0bManifestFamily family,
        std::string source_identity, std::string bytes) noexcept
        -> std::expected<std::optional<MonitoringQ0bProductionInputsLoadResult>, core::Error>;
};
} // namespace epycbench::monitoring::qualification
