#pragma once

#include "epycbench/monitoring/host_environment.hpp"
#include "epycbench/monitoring/host_qualification_plan.hpp"
#include "epycbench/monitoring/host_input_template.hpp"
#include "epycbench/monitoring/q0b_c0_driver.hpp"
#include "epycbench/monitoring/q0b_production_inputs.hpp"
#include "epycbench/runtime/calibration_only_production.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace epycbench::monitoring::qualification {

class detail_HostSessionBinding;
class MonitoringHostQ0bBaseEnvironment;
class MonitoringHostQ0bCharacterization;
class MonitoringHostQ0bBackendResult;
namespace detail {
class detail_HostQ0bV2TestAccess;
struct HostQ0bProjectionAccess;
} // namespace detail

enum class HostQ0bFamily { bandwidth, latency };

class MonitoringHostAdmittedProductionFacts final {
  public:
    MonitoringHostAdmittedProductionFacts(const MonitoringHostAdmittedProductionFacts&) = default;
    MonitoringHostAdmittedProductionFacts(MonitoringHostAdmittedProductionFacts&&) noexcept = default;
    auto operator=(const MonitoringHostAdmittedProductionFacts&)
        -> MonitoringHostAdmittedProductionFacts& = default;
    auto operator=(MonitoringHostAdmittedProductionFacts&&) noexcept
        -> MonitoringHostAdmittedProductionFacts& = default;
    ~MonitoringHostAdmittedProductionFacts() noexcept;
    [[nodiscard]] auto family() const noexcept -> HostQ0bFamily { return family_; }
    [[nodiscard]] auto variant() const noexcept -> runtime::PairwiseVariant { return variant_; }
    [[nodiscard]] auto json() const noexcept -> const nlohmann::ordered_json& { return json_; }
    [[nodiscard]] auto canonical_json() const noexcept -> const std::string& { return canonical_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
  private:
    MonitoringHostAdmittedProductionFacts(HostQ0bFamily family, runtime::PairwiseVariant variant,
        std::shared_ptr<const detail_HostSessionBinding> binding,
        nlohmann::ordered_json json, std::string canonical, core::Sha256Digest digest) noexcept
      : family_{family}, variant_{variant}, json_(std::move(json)), canonical_{std::move(canonical)},
        digest_{digest}, binding_{std::move(binding)} {}
    HostQ0bFamily family_;
    runtime::PairwiseVariant variant_;
    nlohmann::ordered_json json_;
    std::string canonical_;
    core::Sha256Digest digest_;
    std::shared_ptr<const detail_HostSessionBinding> binding_;
    friend auto project_monitoring_host_admitted_production_facts(
        const RunnerContextAcquisitionSession&,const MonitoringHostQualificationPlan&,
        const runtime::CalibrationOnlyProductionAdmission&,
        HostQ0bFamily, runtime::PairwiseVariant) noexcept
        -> std::expected<MonitoringHostAdmittedProductionFacts, core::Error>;
    friend auto project_monitoring_host_q0b_base_environment(
        const HostEnvironmentAuthority&, const MonitoringHostQualificationPlan&,
        const MonitoringHostAdmittedProductionFacts&,
        const MonitoringHostAdmittedProductionFacts&) noexcept
        -> std::expected<MonitoringHostQ0bBaseEnvironment, core::Error>;
    friend auto build_monitoring_host_q0b_characterization(
      std::string_view,std::string_view,const RunnerContextAcquisitionSession&,
      const HostEnvironmentAuthority&,const MonitoringHostQualificationPlan&,
      const MonitoringHostQ0bBaseEnvironment&,const MonitoringHostAdmittedProductionFacts&,
      const MonitoringHostAdmittedProductionFacts&,const Q0bC0Calibrations&) noexcept
      ->std::expected<MonitoringHostQ0bCharacterization,core::Error>;
    friend class detail::detail_HostQ0bV2TestAccess;
    friend struct detail::HostQ0bProjectionAccess;
};

class MonitoringHostQ0bBaseEnvironment final {
  public:
    MonitoringHostQ0bBaseEnvironment(const MonitoringHostQ0bBaseEnvironment&) = default;
    MonitoringHostQ0bBaseEnvironment(MonitoringHostQ0bBaseEnvironment&&) noexcept = default;
    auto operator=(const MonitoringHostQ0bBaseEnvironment&)
        -> MonitoringHostQ0bBaseEnvironment& = default;
    auto operator=(MonitoringHostQ0bBaseEnvironment&&) noexcept
        -> MonitoringHostQ0bBaseEnvironment& = default;
    ~MonitoringHostQ0bBaseEnvironment() noexcept;
    [[nodiscard]] auto json() const noexcept -> const nlohmann::ordered_json& { return json_; }
    [[nodiscard]] auto canonical_json() const noexcept -> const std::string& { return canonical_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
  private:
    MonitoringHostQ0bBaseEnvironment(std::shared_ptr<const detail_HostSessionBinding> binding,
      std::string environment_digest,std::string plan_digest,core::Sha256Digest bandwidth_digest,
      core::Sha256Digest latency_digest,nlohmann::ordered_json json, std::string canonical,
      core::Sha256Digest digest) noexcept
      : json_(std::move(json)), canonical_{std::move(canonical)}, digest_{digest},binding_{std::move(binding)},
        environment_digest_{std::move(environment_digest)},plan_digest_{std::move(plan_digest)},
        bandwidth_digest_{bandwidth_digest},latency_digest_{latency_digest} {}
    nlohmann::ordered_json json_;
    std::string canonical_;
    core::Sha256Digest digest_;
    std::shared_ptr<const detail_HostSessionBinding> binding_;
    std::string environment_digest_,plan_digest_;
    core::Sha256Digest bandwidth_digest_,latency_digest_;
    friend auto project_monitoring_host_q0b_base_environment(
        const HostEnvironmentAuthority&, const MonitoringHostQualificationPlan&,
        const MonitoringHostAdmittedProductionFacts&,
        const MonitoringHostAdmittedProductionFacts&) noexcept
        -> std::expected<MonitoringHostQ0bBaseEnvironment, core::Error>;
    friend auto build_monitoring_host_q0b_characterization(
      std::string_view,std::string_view,const RunnerContextAcquisitionSession&,
      const HostEnvironmentAuthority&,const MonitoringHostQualificationPlan&,
      const MonitoringHostQ0bBaseEnvironment&,const MonitoringHostAdmittedProductionFacts&,
      const MonitoringHostAdmittedProductionFacts&,const Q0bC0Calibrations&) noexcept
      ->std::expected<MonitoringHostQ0bCharacterization,core::Error>;
    friend class detail::detail_HostQ0bV2TestAccess;
    friend struct detail::HostQ0bProjectionAccess;
};

class MonitoringHostQ0bCharacterization final {
 public:
  [[nodiscard]] auto json() const noexcept->const nlohmann::ordered_json&{return json_;}
  [[nodiscard]] auto canonical_json() const noexcept->const std::string&{return canonical_;}
 private:
  MonitoringHostQ0bCharacterization(std::shared_ptr<const detail_HostSessionBinding> binding,
      std::string environment_digest, std::string plan_digest,
      nlohmann::ordered_json json,std::string canonical) noexcept
    :json_(std::move(json)),canonical_{std::move(canonical)},binding_{std::move(binding)},
     environment_digest_{std::move(environment_digest)},plan_digest_{std::move(plan_digest)}{}
  nlohmann::ordered_json json_;std::string canonical_;
  std::shared_ptr<const detail_HostSessionBinding> binding_;
  std::string environment_digest_,plan_digest_;
  friend auto build_monitoring_host_q0b_characterization(
    std::string_view,std::string_view,const RunnerContextAcquisitionSession&,
    const HostEnvironmentAuthority&,const MonitoringHostQualificationPlan&,
    const MonitoringHostQ0bBaseEnvironment&,const MonitoringHostAdmittedProductionFacts&,
    const MonitoringHostAdmittedProductionFacts&,const Q0bC0Calibrations&) noexcept
    ->std::expected<MonitoringHostQ0bCharacterization,core::Error>;
  friend auto build_monitoring_host_q0b_backend_result(
    const HostEnvironmentAuthority&,const MonitoringHostQ0bCharacterization&) noexcept
    ->std::expected<MonitoringHostQ0bBackendResult,core::Error>;
  friend class detail::detail_HostQ0bV2TestAccess;
  friend struct detail::HostQ0bProjectionAccess;
};

class MonitoringHostQ0bBackendResult final {
 public:
  [[nodiscard]] auto json() const noexcept->const nlohmann::ordered_json&{return json_;}
  [[nodiscard]] auto canonical_json() const noexcept->const std::string&{return canonical_;}
 private:
  MonitoringHostQ0bBackendResult(nlohmann::ordered_json json,std::string canonical) noexcept
    :json_(std::move(json)),canonical_{std::move(canonical)}{}
  nlohmann::ordered_json json_;std::string canonical_;
  friend auto build_monitoring_host_q0b_backend_result(
    const HostEnvironmentAuthority&,const MonitoringHostQ0bCharacterization&) noexcept
    ->std::expected<MonitoringHostQ0bBackendResult,core::Error>;
  friend class detail::detail_HostQ0bV2TestAccess;
  friend struct detail::HostQ0bProjectionAccess;
};

[[nodiscard]] auto project_monitoring_host_admitted_production_facts(
    const RunnerContextAcquisitionSession&,const MonitoringHostQualificationPlan&,
    const runtime::CalibrationOnlyProductionAdmission&,
    HostQ0bFamily, runtime::PairwiseVariant) noexcept
    -> std::expected<MonitoringHostAdmittedProductionFacts, core::Error>;
[[nodiscard]] auto project_monitoring_host_q0b_base_environment(
    const HostEnvironmentAuthority&, const MonitoringHostQualificationPlan&,
    const MonitoringHostAdmittedProductionFacts&, const MonitoringHostAdmittedProductionFacts&) noexcept
    -> std::expected<MonitoringHostQ0bBaseEnvironment, core::Error>;

[[nodiscard]] auto build_monitoring_host_q0b_characterization(
    std::string_view qualification_id, std::string_view collected_at,
    const RunnerContextAcquisitionSession&, const HostEnvironmentAuthority&,
    const MonitoringHostQualificationPlan&, const MonitoringHostQ0bBaseEnvironment&,
    const MonitoringHostAdmittedProductionFacts&, const MonitoringHostAdmittedProductionFacts&,
    const Q0bC0Calibrations&) noexcept
    -> std::expected<MonitoringHostQ0bCharacterization, core::Error>;

[[nodiscard]] auto build_monitoring_host_q0b_backend_result(
    const HostEnvironmentAuthority&, const MonitoringHostQ0bCharacterization&) noexcept
    -> std::expected<MonitoringHostQ0bBackendResult, core::Error>;

} // namespace epycbench::monitoring::qualification
