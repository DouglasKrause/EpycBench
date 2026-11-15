#pragma once

#include "monitoring/host_q4_governed_input_authority.hpp"
#include "monitoring/host_q1_git_spawn.hpp"

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ4HistoricalGitFailure final {
    core::Error primary;
    std::array<std::optional<core::Error>, 16> cleanup{};
    std::size_t cleanup_count{};
};

class MonitoringQ4HistoricalGitValue final {
  public:
    MonitoringQ4HistoricalGitValue(const MonitoringQ4HistoricalGitValue&) = delete;
    MonitoringQ4HistoricalGitValue(MonitoringQ4HistoricalGitValue&&) noexcept = default;
    auto operator=(const MonitoringQ4HistoricalGitValue&)
        -> MonitoringQ4HistoricalGitValue& = delete;
    auto operator=(MonitoringQ4HistoricalGitValue&&) -> MonitoringQ4HistoricalGitValue& = delete;
    [[nodiscard]] auto q0_commit() const noexcept -> std::string_view { return commits_[0]; }
    [[nodiscard]] auto q1_commit() const noexcept -> std::string_view { return commits_[1]; }
    [[nodiscard]] auto q2_commit() const noexcept -> std::string_view { return commits_[2]; }
    [[nodiscard]] auto q3_commit() const noexcept -> std::string_view { return commits_[3]; }
    [[nodiscard]] auto current_commit() const noexcept -> std::string_view { return commits_[4]; }
    [[nodiscard]] auto completed_q4_producer_commit() const noexcept -> std::string_view {
        return completed_q4_producer_;
    }

  private:
    MonitoringQ4HistoricalGitValue(std::array<std::string, 5> commits,
                                   std::shared_ptr<const void> binding) noexcept
        : commits_{std::move(commits)}, binding_{std::move(binding)} {}
    std::array<std::string, 5> commits_;
    std::shared_ptr<const void> binding_;
    std::string completed_q4_producer_;
    friend class MonitoringQ4HistoricalGitAuthority;
    friend class MonitoringQ4HistoricalGitAuthorityTestAccess;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticator;
    friend class MonitoringQ4CompleteQ3PredecessorAuthority;
    friend class MonitoringQ4SourceReadyTestAccess;
};

class MonitoringQ4CompleteQ3PredecessorAuthority;
class MonitoringQ4HistoricalGitAuthority final {
  private:
    struct Operations final {
        void* context{};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*object_type)(
            void*, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*ancestry)(
            void*, std::string_view, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*tree)(
            void*, MonitoringQ1GitInvocation, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*blob)(
            void*, std::string_view, const void*, std::size_t, std::size_t, std::size_t){};
        void (*fault)(void*, unsigned int){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*q4_tree)(
            void*, MonitoringQ4HistoricalSelector, std::string_view){};
    };
    struct BootstrapOperations final {
        void* context{};
        MonitoringQ4HistoricalGitFailure (*construct)(void*){};
    };
    static auto authenticate(const MonitoringQ1GitExecutable&,
                             const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                             const MonitoringQ1GitRepositoryMetadata&,
                             const MonitoringQ4GovernedInputCandidate&,
                             const MonitoringQ4CompleteQ3PredecessorAuthority* = nullptr,
                             const std::array<std::string_view, 4>* = nullptr,
                             std::string_view = {})
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto bootstrap_failure() -> MonitoringQ4HistoricalGitFailure;
    static auto authenticate_delegated(Operations, const MonitoringQ4GovernedInputCandidate&,
                                       MonitoringQ4HistoricalGitFailure,
                                       BootstrapOperations) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto authenticate_with(Operations, const MonitoringQ4GovernedInputCandidate&,
                                  MonitoringQ4HistoricalGitFailure) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto authenticate_commits_with(Operations, const MonitoringQ4GovernedInputCandidate&,
                                          const std::array<std::string_view, 4>&,
                                          MonitoringQ4HistoricalGitFailure) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto authenticate_campaign_with(Operations, const MonitoringQ4GovernedInputCandidate&,
        const MonitoringQ4CompleteQ3PredecessorAuthority&, MonitoringQ4HistoricalGitFailure) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto authenticate_completed_with(Operations, const MonitoringQ4GovernedInputCandidate&,
        const MonitoringQ4CompleteQ3PredecessorAuthority&, MonitoringQ4HistoricalGitFailure) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto authenticate_producer_with(Operations, const MonitoringQ4GovernedInputCandidate&,
        MonitoringQ4HistoricalGitValue, std::string_view, MonitoringQ4HistoricalGitFailure) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    static auto authenticate_commits_impl(Operations, const MonitoringQ4GovernedInputCandidate&,
        const std::array<std::string_view, 4>&, MonitoringQ4HistoricalGitFailure, bool) noexcept
        -> std::expected<MonitoringQ4HistoricalGitValue, MonitoringQ4HistoricalGitFailure>;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4HistoricalGitAuthorityTestAccess;
    friend class HostFrozenSourceAdmissionTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
