#pragma once

#include "epycbench/memory/region_placement_query.hpp"
#include "epycbench/platform_linux/page_location.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace epycbench::memory::detail {

inline constexpr std::size_t kPlacementQueryBatchPages = 4096;

struct RegionPlacementQueryOperations {
    void* context;
    std::expected<std::vector<platform_linux::PageQueryStatus>, core::Error> (*query)(
        void* context, std::span<void* const> pages);
};

struct PlacementObservedRegionAccess {
    [[nodiscard]] static auto capacity(
        const RegionPlacementObservation& value) noexcept -> std::size_t {
        return value.resident_page_counts_.capacity();
    }
    [[nodiscard]] static auto take_observation(
        PlacementObservedRegion& value) noexcept
        -> std::optional<RegionPlacementObservation> {
        return std::move(value.observation_);
    }
    [[nodiscard]] static auto region(PlacementObservedRegion& value) noexcept
        -> InitializedRegion& {
        return value.region_;
    }
    [[nodiscard]] static auto region(const PlacementObservedRegion& value) noexcept
        -> const InitializedRegion& {
        return value.region_;
    }

    [[nodiscard]] static auto create_with_operations(
        InitializedRegion region, const RegionPlacementQueryOperations& operations) noexcept
        -> std::expected<PlacementObservedRegion, RegionPlacementQueryFailure>;

    [[nodiscard]] static auto observe_region_placement(
        const InitializedRegion& region,
        const RegionPlacementQueryOperations& operations) noexcept
        -> std::expected<RegionPlacementObservation, core::Error>;

    [[nodiscard]] static constexpr auto batch_size(std::uint64_t total_pages,
                                                   std::uint64_t processed_pages) noexcept
        -> std::size_t {
        if (processed_pages >= total_pages) return 0;
        const auto remaining = total_pages - processed_pages;
        return remaining > kPlacementQueryBatchPages
                   ? kPlacementQueryBatchPages
                   : static_cast<std::size_t>(remaining);
    }

    [[nodiscard]] static constexpr auto batch_count(std::uint64_t total_pages) noexcept
        -> std::uint64_t {
        return total_pages == 0
                   ? 0
                   : 1 + ((total_pages - 1) / kPlacementQueryBatchPages);
    }

  private:
    friend struct PairwiseMemoryAccess;
    [[nodiscard]] static auto node_count(core::LinuxNumaNodeId node,
                                         std::uint64_t pages) noexcept
        -> LinuxNodePageCount {
        return {node, pages};
    }
    [[nodiscard]] static auto observation(
        std::uint64_t total_pages, std::uint64_t resident_pages,
        std::uint64_t unresolved_pages,
        std::vector<LinuxNodePageCount> resident_page_counts) noexcept
        -> RegionPlacementObservation {
        return {total_pages, resident_pages, unresolved_pages,
                std::move(resident_page_counts)};
    }
    [[nodiscard]] static auto success(
        InitializedRegion region, PreRunPlacementQueryState query_state,
        std::optional<RegionPlacementObservation> observation) noexcept
        -> PlacementObservedRegion {
        return {std::move(region), query_state, std::move(observation)};
    }
    [[nodiscard]] static auto failure(core::Error error, InitializedRegion region) noexcept
        -> RegionPlacementQueryFailure {
        return {std::move(error), std::move(region)};
    }
};

} // namespace epycbench::memory::detail
