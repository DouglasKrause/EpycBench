#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/governed_repository_file.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/kernel/kernel_id.hpp"

#include <expected>
#include <filesystem>
#include <string_view>

namespace epycbench::monitoring::qualification {

class MonitoringHostInputTemplateAuthority {
  public:
    MonitoringHostInputTemplateAuthority(const MonitoringHostInputTemplateAuthority&) = delete;
    auto operator=(const MonitoringHostInputTemplateAuthority&)
        -> MonitoringHostInputTemplateAuthority& = delete;
    MonitoringHostInputTemplateAuthority(MonitoringHostInputTemplateAuthority&&) noexcept = default;
    auto operator=(MonitoringHostInputTemplateAuthority&&)
        -> MonitoringHostInputTemplateAuthority& = delete;
    ~MonitoringHostInputTemplateAuthority() noexcept = default;

    [[nodiscard]] auto manifest_file() const noexcept -> const core::GovernedRepositoryFile&;
    [[nodiscard]] auto policy_file() const noexcept -> const core::GovernedRepositoryFile&;
    [[nodiscard]] auto template_file() const noexcept -> const core::GovernedRepositoryFile&;
    [[nodiscard]] auto manifest_sha256() const noexcept -> const core::Sha256Digest&;
    [[nodiscard]] auto policy_sha256() const noexcept -> const core::Sha256Digest&;
    [[nodiscard]] auto template_sha256() const noexcept -> const core::Sha256Digest&;
    [[nodiscard]] auto policy_identity() const noexcept -> std::string_view;
    [[nodiscard]] auto bandwidth_kernel() const noexcept -> kernel::KernelId;
    [[nodiscard]] auto latency_kernel() const noexcept -> kernel::KernelId;
    [[nodiscard]] auto working_set_bytes() const noexcept -> core::ByteCount;
    [[nodiscard]] auto minimum_duration() const noexcept -> core::Duration;
    [[nodiscard]] auto target_duration() const noexcept -> core::Duration;
    [[nodiscard]] auto maximum_duration() const noexcept -> core::Duration;
    [[nodiscard]] auto uses_worker_local_memory() const noexcept -> bool;
    [[nodiscard]] auto monitoring_configuration() const noexcept -> std::string_view;

  private:
    MonitoringHostInputTemplateAuthority(
        core::GovernedRepositoryFile manifest, core::GovernedRepositoryFile policy,
        core::GovernedRepositoryFile input_template, core::Sha256Digest manifest_sha,
        core::Sha256Digest policy_sha, core::Sha256Digest template_sha,
        core::Duration minimum_duration, core::Duration target_duration,
        core::Duration maximum_duration) noexcept;

    core::GovernedRepositoryFile manifest_;
    core::GovernedRepositoryFile policy_;
    core::GovernedRepositoryFile template_;
    const core::Sha256Digest manifest_sha_;
    const core::Sha256Digest policy_sha_;
    const core::Sha256Digest template_sha_;
    const core::Duration minimum_duration_;
    const core::Duration target_duration_;
    const core::Duration maximum_duration_;

    friend class detail_MonitoringHostInputTemplateFactory;
};

[[nodiscard]] auto
load_monitoring_host_input_template(const std::filesystem::path& repository_root) noexcept
    -> std::expected<MonitoringHostInputTemplateAuthority, core::Error>;

} // namespace epycbench::monitoring::qualification
