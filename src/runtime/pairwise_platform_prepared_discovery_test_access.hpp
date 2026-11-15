#pragma once

#include "runtime/pairwise_platform_leaf_facade.hpp"

#include <cstddef>

namespace epycbench::runtime::detail {

template <class Lower>
struct PairwisePlatformPreparedDiscoveryObservation final {
    const Lower* lower{};
    const void* mapped_cleanup{};
    std::size_t mapped_cleanup_size{};
    std::size_t mapped_cleanup_capacity{};
    bool consumed{};
};

class PairwisePlatformPreparedDiscoveryTestAccess final {
  public:
    [[nodiscard]] static auto observe(
        const PairwisePreparedHwmonDiscoveryStorage& storage) noexcept
        -> PairwisePlatformPreparedDiscoveryObservation<
            platform_linux::detail::PreparedBoundedHwmonDiscoveryStorage> {
        return {
            .lower = &storage.lower_,
            .mapped_cleanup = storage.mapped_cleanup_.data(),
            .mapped_cleanup_size = storage.mapped_cleanup_.size(),
            .mapped_cleanup_capacity = storage.mapped_cleanup_.capacity(),
            .consumed = storage.consumed_};
    }

    [[nodiscard]] static auto observe(
        const PairwisePreparedCpuFrequencyDiscoveryStorage& storage) noexcept
        -> PairwisePlatformPreparedDiscoveryObservation<
            platform_linux::detail::PreparedBoundedCpuFrequencyDiscoveryStorage> {
        return {
            .lower = &storage.lower_,
            .mapped_cleanup = storage.mapped_cleanup_.data(),
            .mapped_cleanup_size = storage.mapped_cleanup_.size(),
            .mapped_cleanup_capacity = storage.mapped_cleanup_.capacity(),
            .consumed = storage.consumed_};
    }
};

} // namespace epycbench::runtime::detail
