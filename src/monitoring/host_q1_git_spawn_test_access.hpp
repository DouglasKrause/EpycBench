#pragma once
#include "monitoring/host_q1_git_spawn.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1GitSpawnTestAccess final {
  public:
    static auto valid_fixed_path(std::string_view path) noexcept {
        return MonitoringQ1GitSpawn::validate_fixed_path(path);
    }
    static auto valid_blob_token(const MonitoringQ1GitObjectId& object, const void* binding,
                                 std::size_t width, std::size_t slot) noexcept {
        return MonitoringQ1GitSpawn::validate_blob_token(object, binding, width, slot);
    }
    static auto run_tree_path(const MonitoringQ1GitExecutable& executable,
                              const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                              std::string_view path,
                              const MonitoringQ1GitSpawnOperations& operations) {
        return MonitoringQ1GitSpawn::run_tree_path_with_operations(executable, root, path,
                                                                   operations);
    }
    static auto run(const MonitoringQ1GitExecutable& executable,
                    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                    MonitoringQ1GitInvocation invocation,
                    const MonitoringQ1GitSpawnOperations& operations) {
        return MonitoringQ1GitSpawn::run_with_operations(executable, root, invocation, operations);
    }
    static auto run_historical_tree(const MonitoringQ1GitExecutable& executable,
                                    const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                                    MonitoringQ1GitInvocation invocation,
                                    std::string_view commit,
                                    const MonitoringQ1GitSpawnOperations& operations) {
        return MonitoringQ1GitSpawn::run_with_operations(executable, root, invocation,
                                                         operations, commit);
    }
    static auto run_blob(const MonitoringQ1GitExecutable& executable,
                         const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                         const MonitoringQ1GitObjectId& object, const void* binding,
                         std::size_t width, std::size_t slot) {
        return MonitoringQ1GitSpawn::run_blob(executable, root, object, binding, width, slot);
    }
    static auto run_blob(const MonitoringQ1GitExecutable& executable,
                         const core::detail::OwnedGovernedRepositoryRootDescriptor& root,
                         const MonitoringQ1GitObjectId& object, const void* binding,
                         std::size_t width, std::size_t slot,
                         const MonitoringQ1GitSpawnOperations& operations) {
        return MonitoringQ1GitSpawn::run_blob_with_operations(executable, root, object, binding,
                                                              width, slot, operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
