#pragma once

#include <nlohmann/json_fwd.hpp>
#include "epycbench/core/digest.hpp"
#include <string>
#include <memory>

namespace epycbench::monitoring::qualification {
class MonitoringHostAdmittedProductionFacts;
class MonitoringHostQ0bBaseEnvironment;
class MonitoringHostQ0bBackendResult;
class detail_HostSessionBinding;
}

namespace epycbench::monitoring::qualification::detail {

class detail_HostQ0bV2TestAccess;

// Noninstalled, test-only fault controls. Production callers cannot obtain or
// mutate any qualification capability through this seam.
class detail_HostQ0bV2TestAccess final {
 public:
  enum class Fault { none, payload_standard_exception, payload_unknown_exception,
                     payload_invalid, backend_oversize, backend_privacy };
  static void set_build_override(nlohmann::json);
  // Explicit synthetic repository input only: retains canonical Build validation,
  // without claiming an association to the executable's compiled revision.
  static void set_synthetic_repository_build_override(nlohmann::json);
  static void clear_build_override() noexcept;
  static void set_fault(Fault) noexcept;
  static auto fault() noexcept -> Fault;
  static auto mutable_json(MonitoringHostAdmittedProductionFacts&) noexcept
      -> nlohmann::ordered_json&;
  static void mutate_canonical(MonitoringHostAdmittedProductionFacts&,std::string);
  static void mutate_digest(MonitoringHostAdmittedProductionFacts&,core::Sha256Digest) noexcept;
  static void mutate_binding(MonitoringHostQ0bBaseEnvironment&,
      std::shared_ptr<const detail_HostSessionBinding>) noexcept;
  static void mutate_environment_digest(MonitoringHostQ0bBaseEnvironment&,std::string);
  static void mutate_plan_digest(MonitoringHostQ0bBaseEnvironment&,std::string);
  static void mutate_family_digests(MonitoringHostQ0bBaseEnvironment&,
      core::Sha256Digest,core::Sha256Digest) noexcept;
  static auto make_backend_result(nlohmann::ordered_json,std::string) noexcept
      -> MonitoringHostQ0bBackendResult;
};

class HostQ0bV2TestGuard final {
 public:
  HostQ0bV2TestGuard() = default;
  HostQ0bV2TestGuard(const HostQ0bV2TestGuard&) = delete;
  auto operator=(const HostQ0bV2TestGuard&) -> HostQ0bV2TestGuard& = delete;
  ~HostQ0bV2TestGuard() noexcept {
    detail_HostQ0bV2TestAccess::set_fault(detail_HostQ0bV2TestAccess::Fault::none);
    detail_HostQ0bV2TestAccess::clear_build_override();
  }
};

} // namespace epycbench::monitoring::qualification::detail
