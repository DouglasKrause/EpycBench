#pragma once

#include "epycbench/memory/region_binding.hpp"
#include "epycbench/platform_linux/numa_policy.hpp"
#include "platform_linux/numa_operations.hpp"

#include <expected>

namespace epycbench::memory::detail {

struct RegionBindingOperations {
    void* context;
    std::expected<platform_linux::NativeNumaMask, core::Error> (*create_mask)(
        void* context, const core::LinuxNumaNodeSet& nodes);
    std::expected<void, core::Error> (*bind)(
        void* context, const platform_linux::AnonymousMapping& mapping,
        const platform_linux::NativeNumaMask& mask);
    std::expected<void, platform_linux::detail::NumaBindFailure> (*bind_diagnosed)(
        void*, const platform_linux::AnonymousMapping&,
        const platform_linux::NativeNumaMask&,
        platform_linux::detail::NumaDomainMaxNode,
        const platform_linux::detail::NumaBindInvocationFacts&,
        platform_linux::detail::AllowedMemsWorkspace&) noexcept{};
    platform_linux::detail::NumaDomainMaxNode* domain{};
    platform_linux::detail::AllowedMemsWorkspace* workspace{};
    const platform_linux::detail::NumaBindInvocationFacts* invocation_facts{};
    void (*before_diagnosed_projection)(void*){};
};

struct BoundRegionAccess {
    [[nodiscard]] static auto mapping(BoundRegion& region) noexcept -> RegionMapping& {
        return region.mapping_;
    }
    [[nodiscard]] static auto mapping(const BoundRegion& region) noexcept
        -> const RegionMapping& {
        return region.mapping_;
    }

    [[nodiscard]] static auto create_with_operations(
        RegionMapping mapping, const RegionBindingOperations& operations) noexcept
        -> std::expected<BoundRegion, RegionBindingFailure>;

  private:
    friend struct PairwiseMemoryAccess;
    [[nodiscard]] static auto evidence() noexcept -> RegionBindingEvidence { return {}; }
    [[nodiscard]] static auto success(RegionMapping mapping,
                                      RegionBindingEvidence evidence) noexcept -> BoundRegion {
        return BoundRegion{std::move(mapping), evidence};
    }
    [[nodiscard]] static auto failure(core::Error error,
                                      RegionMapping mapping) noexcept -> RegionBindingFailure {
        return RegionBindingFailure{std::move(error), std::move(mapping)};
    }
};

} // namespace epycbench::memory::detail
