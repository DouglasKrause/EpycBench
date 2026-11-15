#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/governed_repository_file.hpp"
#include "epycbench/scenario/kernel_binding_projection.hpp"
#include "epycbench/scenario/structural_validation.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::monitoring::qualification {

class MonitoringQ0bProductionInputFamilyAuthority {
  public:
    [[nodiscard]] auto manifest_family() const noexcept -> std::string_view { return family_; }
    [[nodiscard]] auto scenario_path() const noexcept -> const std::filesystem::path& { return file_.repository_relative_path(); }
    [[nodiscard]] auto declared_sha256() const noexcept -> const core::Sha256Digest& { return declared_sha_; }
    [[nodiscard]] auto actual_sha256() const noexcept -> const core::Sha256Digest& { return actual_sha_; }
    [[nodiscard]] auto file() const noexcept -> const core::GovernedRepositoryFile& { return file_; }
    [[nodiscard]] auto scenario() const noexcept -> const scenario::StructurallyValidScenario& { return scenario_; }
    [[nodiscard]] auto bindings() const noexcept -> const scenario::ScenarioKernelBindings& { return bindings_; }

  private:
    MonitoringQ0bProductionInputFamilyAuthority(std::string family, core::Sha256Digest declared_sha,
        core::Sha256Digest actual_sha, core::GovernedRepositoryFile file,
        scenario::StructurallyValidScenario scenario, scenario::ScenarioKernelBindings bindings)
        : family_{std::move(family)}, declared_sha_{declared_sha}, actual_sha_{actual_sha},
          file_{std::move(file)}, scenario_{std::move(scenario)}, bindings_{std::move(bindings)} {}
    std::string family_;
    core::Sha256Digest declared_sha_;
    core::Sha256Digest actual_sha_;
    core::GovernedRepositoryFile file_;
    scenario::StructurallyValidScenario scenario_;
    scenario::ScenarioKernelBindings bindings_;
    friend class detail_MonitoringQ0bProductionInputsFactory;
};

class MonitoringQ0bProductionInputsAuthority {
  public:
    [[nodiscard]] auto manifest_file() const noexcept -> const core::GovernedRepositoryFile& { return manifest_file_; }
    [[nodiscard]] auto manifest_raw_sha256() const noexcept -> const core::Sha256Digest& { return manifest_sha_; }
    [[nodiscard]] auto schema() const noexcept -> std::string_view { return schema_; }
    [[nodiscard]] auto policy_identity() const noexcept -> std::string_view { return policy_identity_; }
    [[nodiscard]] auto policy_sha256() const noexcept -> const core::Sha256Digest& { return policy_sha_; }
    [[nodiscard]] auto bandwidth() const noexcept -> const MonitoringQ0bProductionInputFamilyAuthority& { return bandwidth_; }
    [[nodiscard]] auto latency() const noexcept -> const MonitoringQ0bProductionInputFamilyAuthority& { return latency_; }

  private:
    MonitoringQ0bProductionInputsAuthority(core::GovernedRepositoryFile manifest_file,
        core::Sha256Digest manifest_sha, std::string schema, std::string policy_identity,
        core::Sha256Digest policy_sha, MonitoringQ0bProductionInputFamilyAuthority bandwidth,
        MonitoringQ0bProductionInputFamilyAuthority latency)
        : manifest_file_{std::move(manifest_file)}, manifest_sha_{manifest_sha}, schema_{std::move(schema)},
          policy_identity_{std::move(policy_identity)}, policy_sha_{policy_sha},
          bandwidth_{std::move(bandwidth)}, latency_{std::move(latency)} {}
    core::GovernedRepositoryFile manifest_file_;
    core::Sha256Digest manifest_sha_;
    std::string schema_;
    std::string policy_identity_;
    core::Sha256Digest policy_sha_;
    MonitoringQ0bProductionInputFamilyAuthority bandwidth_;
    MonitoringQ0bProductionInputFamilyAuthority latency_;
    friend class detail_MonitoringQ0bProductionInputsFactory;
};

class MonitoringQ0bProductionInputsLoadResult {
  public:
    [[nodiscard]] auto authority() const noexcept -> const std::optional<MonitoringQ0bProductionInputsAuthority>& { return authority_; }
    [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<scenario::ScenarioDiagnostic>& { return diagnostics_; }
  private:
    explicit MonitoringQ0bProductionInputsLoadResult(MonitoringQ0bProductionInputsAuthority authority)
        : authority_{std::move(authority)} {}
    MonitoringQ0bProductionInputsLoadResult(scenario::ScenarioDiagnostic first,
        std::vector<scenario::ScenarioDiagnostic> remaining)
        : diagnostics_{std::move(remaining)} {
        diagnostics_.insert(diagnostics_.begin(), std::move(first));
    }
    std::optional<MonitoringQ0bProductionInputsAuthority> authority_;
    std::vector<scenario::ScenarioDiagnostic> diagnostics_;
    friend class detail_MonitoringQ0bProductionInputsFactory;
    friend class detail_MonitoringQ0bProductionInputsAccess;
};

[[nodiscard]] auto load_q0b_production_inputs(const std::filesystem::path& repository_root) noexcept
    -> std::expected<MonitoringQ0bProductionInputsLoadResult, core::Error>;

} // namespace epycbench::monitoring::qualification
