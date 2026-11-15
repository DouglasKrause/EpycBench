#pragma once

#include "epycbench/memory/retained_region_cleanup.hpp"

namespace epycbench::memory::detail {

struct RetainedRegionCleanupAccess {
    [[nodiscard]] static auto take_error(RegionMappingCleanupFailure& value) noexcept
        -> core::Error { return std::move(value.error_); }
    [[nodiscard]] static auto take_error(BoundRegionCleanupFailure& value) noexcept
        -> core::Error { return std::move(value.error_); }
    [[nodiscard]] static auto take_error(InitializedRegionCleanupFailure& value) noexcept
        -> core::Error { return std::move(value.error_); }
    [[nodiscard]] static auto take_error(PlacementObservedRegionCleanupFailure& value) noexcept
        -> core::Error { return std::move(value.error_); }
    [[nodiscard]] static auto evidence(bool attempted, bool succeeded,
                                       bool active_after) noexcept
        -> RegionCleanupEvidence {
        return {attempted, succeeded, active_after};
    }
    [[nodiscard]] static auto success(RetainedRegionSourceState source,
                                      RegionCleanupEvidence evidence) noexcept
        -> CleanedRetainedRegion { return {source, evidence}; }

    [[nodiscard]] static auto failure(core::Error error, RegionCleanupEvidence evidence,
                                      RetainedRegionSourceState source,
                                      RegionMapping owner) noexcept
        -> RegionMappingCleanupFailure {
        return {std::move(error), evidence, source, std::move(owner)};
    }
    [[nodiscard]] static auto failure(core::Error error, RegionCleanupEvidence evidence,
                                      RetainedRegionSourceState source,
                                      BoundRegion owner) noexcept
        -> BoundRegionCleanupFailure {
        return {std::move(error), evidence, source, std::move(owner)};
    }
    [[nodiscard]] static auto failure(core::Error error, RegionCleanupEvidence evidence,
                                      RetainedRegionSourceState source,
                                      InitializedRegion owner) noexcept
        -> InitializedRegionCleanupFailure {
        return {std::move(error), evidence, source, std::move(owner)};
    }
    [[nodiscard]] static auto failure(core::Error error, RegionCleanupEvidence evidence,
                                      RetainedRegionSourceState source,
                                      PlacementObservedRegion owner) noexcept
        -> PlacementObservedRegionCleanupFailure {
        return {std::move(error), evidence, source, std::move(owner)};
    }

    [[nodiscard]] static auto owner(const RegionMappingCleanupFailure& value) noexcept
        -> const RegionMapping& { return value.owner_; }
    [[nodiscard]] static auto owner(const BoundRegionCleanupFailure& value) noexcept
        -> const BoundRegion& { return value.owner_; }
    [[nodiscard]] static auto owner(const InitializedRegionCleanupFailure& value) noexcept
        -> const InitializedRegion& { return value.owner_; }
    [[nodiscard]] static auto owner(
        const PlacementObservedRegionCleanupFailure& value) noexcept
        -> const PlacementObservedRegion& { return value.owner_; }
};

} // namespace epycbench::memory::detail
