#pragma once

#include "epycbench/runtime/linux_pairwise_production_host_operations_provider.hpp"
#include "../platform_linux/numa_operations.hpp"

#include <optional>

namespace epycbench::runtime {

struct LinuxPairwiseProductionHostOperationsProvider::Dependencies {
    PairwiseProductionHostOperations host;
    std::shared_ptr<void> monitoring_owner;
    std::optional<platform_linux::detail::NumaOperations> diagnostic_numa;
};

namespace detail {
class LinuxPairwiseProductionHostOperationsProviderAccess {
  public:
    using Dependencies = LinuxPairwiseProductionHostOperationsProvider::Dependencies;
    [[nodiscard]] static auto create(Dependencies dependencies)
        -> LinuxPairwiseProductionHostOperationsProvider;
};
} // namespace detail
} // namespace epycbench::runtime
