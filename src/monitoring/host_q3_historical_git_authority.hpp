#pragma once

#include "monitoring/host_q1_git_fixed_results.hpp"

#include <array>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ3HistoricalGitFailure final {
    core::Error primary;
    std::array<std::optional<core::Error>, 16> cleanup{};
    std::size_t cleanup_count{};
};

class MonitoringQ3HistoricalGitAuthorityTestAccess;

class MonitoringQ3HistoricalGitAuthority final {
  public:
    [[nodiscard]] static auto authenticate(
        const MonitoringQ1GitExecutable&, const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        const MonitoringQ1GitRepositoryMetadata&, std::string_view q0_commit,
        std::string_view q1_commit, std::string_view q2_commit, std::string_view current_commit,
        std::string_view profile_bytes, std::string_view q1_plan_bytes,
        std::string_view q2_plan_bytes, std::string_view q3_plan_bytes) noexcept
        -> std::expected<void, MonitoringQ3HistoricalGitFailure>;

  private:
    struct Operations final {
        void* context{};
        std::expected<MonitoringQ1GitFixedResults, MonitoringQ1GitCommandFailure> (*capture)(void*){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*object_type)(
            void*, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*ancestry)(
            void*, std::string_view, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*tree)(
            void*, MonitoringQ1GitInvocation, std::string_view){};
        std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure> (*blob)(
            void*, std::string_view, const void*, std::size_t, std::size_t){};
        void (*fault)(void*, unsigned int){};
    };
    [[nodiscard]] static auto authenticate_with(
        Operations, std::string_view, std::string_view, std::string_view, std::string_view,
        std::string_view, std::string_view, std::string_view, std::string_view) noexcept
        -> std::expected<void, MonitoringQ3HistoricalGitFailure>;
    [[nodiscard]] static auto current_matches(
        const MonitoringQ1GitFixedResults&, std::string_view,
        const std::array<std::string_view, 4>&) noexcept -> bool;
    friend class MonitoringQ3HistoricalGitAuthorityTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
