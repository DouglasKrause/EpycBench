#pragma once
#include "core/governed_repository_root_authority_access.hpp"
#include "monitoring/host_q1_git_bounded_child.hpp"

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ1GitObjectId final {
  public:
    MonitoringQ1GitObjectId(const MonitoringQ1GitObjectId&) = delete;
    auto operator=(const MonitoringQ1GitObjectId&) -> MonitoringQ1GitObjectId& = delete;
    MonitoringQ1GitObjectId(MonitoringQ1GitObjectId&&) noexcept = default;
    auto operator=(MonitoringQ1GitObjectId&&) -> MonitoringQ1GitObjectId& = delete;

  private:
    MonitoringQ1GitObjectId(std::string value, const void* binding, std::size_t width,
                            std::size_t slot, std::size_t stdout_limit = 1U << 20U)
        : value_{std::move(value)}, binding_{binding}, width_{width}, slot_{slot},
          stdout_limit_{stdout_limit} {}
    std::string value_;
    const void* binding_{};
    std::size_t width_{};
    std::size_t slot_{};
    std::size_t stdout_limit_{};
    friend class MonitoringQ1GitFixedResultAuthority;
    friend class MonitoringQ1GitFixedResultAuthorityTestAccess;
    friend class MonitoringQ1GitSpawn;
    friend class MonitoringQ2HistoricalQ1GitAuthority;
    friend class MonitoringQ3HistoricalGitAuthority;
    friend class MonitoringQ4HistoricalGitAuthority;
    friend class MonitoringCurrentRevisionTrackedCaptureCore;
    friend class MonitoringQ4CurrentRevisionTrackedCapture;
    friend class MonitoringCurrentRevisionTrackedCaptureTestAccess;
};

enum class MonitoringQ1GitInvocation : unsigned char {
    head_commit,
    clean_status,
    tree_policy,
    tree_profile,
    tree_plan,
    tree_q2_plan,
    tree_q3_plan,
    historical_tree_profile,
    historical_tree_q1_plan,
    historical_tree_q2_plan,
    historical_tree_q3_plan,
    exact_object_type,
    historical_ancestry
};
enum class MonitoringQ4HistoricalSelector : unsigned char {
    plan, plan_schema, evidence_schema, result_schema, error_schema, authored_schema
};
struct MonitoringQ1GitSpawnOperations final {
    void* context{};
    int (*pipe)(void*, int*, int) noexcept {};
    int (*open_null)(void*) noexcept {};
    int (*fork)(void*) noexcept {};
    int (*change_directory)(void*, int) noexcept {};
    int (*duplicate)(void*, int, int) noexcept {};
    int (*close_range)(void*) noexcept {};
    int (*execute)(void*, int, char* const*, char* const*) noexcept {};
    long (*write_setup)(void*, int, const void*, std::size_t) noexcept {};
    void (*exit_child)(void*, int) noexcept {};
    int (*close_descriptor)(void*, int) noexcept {};
    MonitoringQ1GitBoundedChildOperations child_operations{};
};

class MonitoringQ1GitSpawn final {
  public:
    [[nodiscard]] static auto run(const MonitoringQ1GitExecutable&,
                                  const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                  MonitoringQ1GitInvocation)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;

  private:
    // Shared fixed argument algorithm; source supervision calls it only after
    // closed-record validation and the parent-owned original launch scope.
    [[nodiscard]] static auto build_arguments(MonitoringQ1GitInvocation,
        char* object, char* secondary, char* path) noexcept -> std::array<char*, 15>;
    friend struct HostFrozenSourceLaunchState;
    friend class HostFrozenSourceAdmissionTestAccess;
    friend class MonitoringQ1GitSpawnTestAccess;
    friend class MonitoringQ1GitFixedResultAuthority;
    friend class MonitoringQ1GitFixedResultAuthorityTestAccess;
    friend class MonitoringQ2HistoricalQ1GitAuthority;
    friend class MonitoringQ3HistoricalGitAuthority;
    friend class MonitoringQ4HistoricalGitAuthority;
    friend class MonitoringCurrentRevisionTrackedCaptureCore;
    friend class MonitoringQ4CurrentRevisionTrackedCapture;
    [[nodiscard]] static auto run_blob(const MonitoringQ1GitExecutable&,
                                       const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                       const MonitoringQ1GitObjectId&, const void*, std::size_t,
                                       std::size_t)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_historical_tree(const MonitoringQ1GitExecutable&,
                        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                        MonitoringQ1GitInvocation, std::string_view)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto run_q4_historical_tree(
        const MonitoringQ1GitExecutable&,
        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        MonitoringQ4HistoricalSelector, std::string_view)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto run_q4_historical_tree_with_operations(
        const MonitoringQ1GitExecutable&,
        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        MonitoringQ4HistoricalSelector, std::string_view,
        const MonitoringQ1GitSpawnOperations&)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_historical_ancestry(const MonitoringQ1GitExecutable&,
                            const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                            std::string_view, std::string_view)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_exact_object_type(const MonitoringQ1GitExecutable&,
                          const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                          std::string_view)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_tree_path(const MonitoringQ1GitExecutable&,
                  const core::detail::OwnedGovernedRepositoryRootDescriptor&, std::string_view)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto validate_fixed_path(std::string_view) noexcept -> bool;
    [[nodiscard]] static auto
    run_tree_path_with_operations(const MonitoringQ1GitExecutable&,
                                  const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                  std::string_view, const MonitoringQ1GitSpawnOperations&)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto validate_blob_token(const MonitoringQ1GitObjectId&, const void*,
                                                  std::size_t, std::size_t) noexcept -> bool;
    [[nodiscard]] static auto
    run_blob_with_operations(const MonitoringQ1GitExecutable&,
                             const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                             const MonitoringQ1GitObjectId&, const void*, std::size_t, std::size_t,
                             const MonitoringQ1GitSpawnOperations&)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] static auto
    run_with_operations(const MonitoringQ1GitExecutable&,
                        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                        MonitoringQ1GitInvocation, const MonitoringQ1GitSpawnOperations&,
                        std::string_view blob_id = {}, std::string_view secondary_id = {},
                        std::string_view fixed_path = {}, std::size_t stdout_limit = 1U << 20U)
        -> std::expected<MonitoringQ1GitBoundedOutput, MonitoringQ1GitCommandFailure>;
};

} // namespace epycbench::monitoring::qualification::detail
