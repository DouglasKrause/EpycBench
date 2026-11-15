#pragma once
#include "monitoring/host_q1_git_repository_metadata.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1GitRepositoryMetadataTestAccess final {
  public:
    using Operations = MonitoringQ1GitRepositoryMetadata::Operations;
    [[nodiscard]] static auto retain(int root, Operations operations) {
        return MonitoringQ1GitRepositoryMetadata::retain_with(root, operations);
    }
};
} // namespace epycbench::monitoring::qualification::detail
