#pragma once

#include "epycbench/memory/region_mapping.hpp"

#include <cstddef>
#include <expected>

namespace epycbench::memory::detail {

struct RegionMappingOperations {
    void* context;
    std::expected<platform_linux::AnonymousMapping, core::Error> (*create_mapping)(
        void* context, std::size_t mapping_bytes);
};

struct RegionMappingAccess {
    [[nodiscard]] static auto take_request(RegionMapping& region) noexcept
        -> RegionRequest { return std::move(region.request_); }
    [[nodiscard]] static auto mapping(RegionMapping& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return region.mapping_;
    }

    [[nodiscard]] static auto mapping(const RegionMapping& region) noexcept
        -> const platform_linux::AnonymousMapping& {
        return region.mapping_;
    }

    [[nodiscard]] static auto calculate_mapping_bytes(core::ByteCount requested,
                                                      core::ByteCount page) noexcept
        -> std::expected<std::size_t, core::Error>;

    [[nodiscard]] static auto
    create_with_operations(RegionRequest request,
                           const RegionMappingOperations& operations) noexcept
        -> std::expected<RegionMapping, RegionMappingFailure>;

    [[nodiscard]] static auto create_for_test(RegionRequest request,
                                              platform_linux::AnonymousMapping mapping,
                                              std::size_t mapping_bytes) noexcept
        -> RegionMapping {
        return {std::move(request), std::move(mapping), mapping_bytes};
    }

  private:
    friend struct PairwiseMemoryAccess;
    [[nodiscard]] static auto success(RegionRequest request,
                                      platform_linux::AnonymousMapping mapping,
                                      std::size_t mapping_bytes) noexcept
        -> RegionMapping {
        return {std::move(request), std::move(mapping), mapping_bytes};
    }
};

} // namespace epycbench::memory::detail
