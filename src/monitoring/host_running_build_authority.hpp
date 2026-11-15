#pragma once
#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
namespace epycbench::monitoring::qualification::detail {
class MonitoringRunningBuildAuthorityTestAccess;
class MonitoringRunningBuildAuthority final {
 public:
  struct Operations final{void* context{};nlohmann::json (*manifest)(void*);void (*fault)(void*,unsigned int);
    std::optional<core::Error> (*recheck_failure)(void*){};};
  MonitoringRunningBuildAuthority(const MonitoringRunningBuildAuthority&)=delete;
  MonitoringRunningBuildAuthority(MonitoringRunningBuildAuthority&&) noexcept;
  auto operator=(MonitoringRunningBuildAuthority&&) -> MonitoringRunningBuildAuthority& = delete;
  ~MonitoringRunningBuildAuthority() noexcept;
  static auto retain(std::string_view) noexcept
    ->std::expected<MonitoringRunningBuildAuthority,core::Error>;
  static auto retain_for_composition(std::string_view commit,Operations operations) noexcept
    ->std::expected<MonitoringRunningBuildAuthority,core::Error> {
    return retain_with(commit,operations);
  }
  auto recheck() noexcept->std::expected<bool,core::Error>;
  auto retire() noexcept->std::expected<void,core::Error>;
  [[nodiscard]] auto has_value() const noexcept -> bool { return active_; }
  [[nodiscard]] auto identity() const noexcept -> const core::Sha256Digest& { return identity_; }
  [[nodiscard]] auto canonical() const noexcept -> std::string_view { return canonical_; }
 private:
  static auto retain_with(std::string_view,Operations) noexcept
    ->std::expected<MonitoringRunningBuildAuthority,core::Error>;
  MonitoringRunningBuildAuthority(std::string,std::string,core::Sha256Digest,Operations,
    core::Error,core::Error,core::Error) noexcept;
  std::string commit_,canonical_;core::Sha256Digest identity_;Operations operations_{};
  core::Error recheck_error_,retire_error_,contained_error_;bool active_{true},checked_{};
  friend class MonitoringRunningBuildAuthorityTestAccess;
};
}
