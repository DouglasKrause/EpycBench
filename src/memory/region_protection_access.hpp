#pragma once

#include "epycbench/memory/region_protection.hpp"
#include "epycbench/platform_linux/memory_mapping.hpp"

#include <expected>
#include <span>
#include <utility>

namespace epycbench::memory::detail {

struct RegionProtectionOperations {
    void* context;
    std::expected<void, core::Error> (*protect_read_only)(
        void* context, platform_linux::AnonymousMapping& mapping);
};

struct ReadOnlyRegionAccess {
    [[nodiscard]] static auto region(ReadOnlyRegion& value) noexcept
        -> PlacementEvaluatedRegion& {
        return value.region_;
    }
    [[nodiscard]] static auto region(const ReadOnlyRegion& value) noexcept
        -> const PlacementEvaluatedRegion& {
        return value.region_;
    }
    [[nodiscard]] static auto requested_region(const ReadOnlyRegion& value) noexcept
        -> std::span<const std::byte> {
        return value.requested_region();
    }

    [[nodiscard]] static auto create_with_operations(
        PlacementEvaluatedRegion region,
        const RegionProtectionOperations& operations) noexcept
        -> std::expected<ReadOnlyRegion, RegionProtectionFailure>;

  private:
    friend struct PairwiseMemoryAccess;
    [[nodiscard]] static auto evidence() noexcept -> RegionProtectionEvidence {
        return {};
    }
    [[nodiscard]] static auto success(
        PlacementEvaluatedRegion region,
        RegionProtectionEvidence evidence) noexcept -> ReadOnlyRegion {
        return {std::move(region), evidence};
    }
    [[nodiscard]] static auto failure(
        core::Error error,
        PlacementEvaluatedRegion region) noexcept -> RegionProtectionFailure {
        return {std::move(error), std::move(region)};
    }
};

} // namespace epycbench::memory::detail
