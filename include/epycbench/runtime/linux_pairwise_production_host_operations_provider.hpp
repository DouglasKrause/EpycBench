#pragma once

#include "epycbench/runtime/pairwise_production_operations_adapter.hpp"

#include <memory>

namespace epycbench::runtime {

namespace detail { class LinuxPairwiseProductionHostOperationsProviderAccess; }

class LinuxPairwiseProductionHostOperationsProvider {
  public:
    struct Context;
    struct Dependencies;

    LinuxPairwiseProductionHostOperationsProvider(
        LinuxPairwiseProductionHostOperationsProvider&&) noexcept;
    auto operator=(LinuxPairwiseProductionHostOperationsProvider&&)
        -> LinuxPairwiseProductionHostOperationsProvider& = delete;
    LinuxPairwiseProductionHostOperationsProvider(
        const LinuxPairwiseProductionHostOperationsProvider&) = delete;
    auto operator=(const LinuxPairwiseProductionHostOperationsProvider&)
        -> LinuxPairwiseProductionHostOperationsProvider& = delete;
    ~LinuxPairwiseProductionHostOperationsProvider() noexcept;

    // Returned tables borrow provider-owned callback state. The provider must outlive every
    // adapter, backend, production run, and finalization using a copied table.
    [[nodiscard]] auto operations() const noexcept -> PairwiseProductionHostOperations;
    // The canonical adapter retains the callback state independently of this provider object.
    [[nodiscard]] auto adapter() const -> PairwiseProductionOperationsAdapter;

  private:
    explicit LinuxPairwiseProductionHostOperationsProvider(std::shared_ptr<Context>) noexcept;
    std::shared_ptr<Context> context_;

    friend class detail::LinuxPairwiseProductionHostOperationsProviderAccess;
    friend auto make_linux_pairwise_production_host_operations_provider()
        -> LinuxPairwiseProductionHostOperationsProvider;
};

// Structural only: no callback is invoked and no host state is inspected or changed.
[[nodiscard]] auto make_linux_pairwise_production_host_operations_provider()
    -> LinuxPairwiseProductionHostOperationsProvider;

} // namespace epycbench::runtime
