#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/memory/region_initialization.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::memory {

namespace detail {
struct PlacementObservedRegionAccess;
}

class LinuxNodePageCount {
  public:
    [[nodiscard]] auto linux_node() const noexcept -> core::LinuxNumaNodeId {
        return linux_node_;
    }
    [[nodiscard]] auto page_count() const noexcept -> std::uint64_t { return page_count_; }

    auto operator==(const LinuxNodePageCount&) const noexcept -> bool = default;

  private:
    LinuxNodePageCount(core::LinuxNumaNodeId linux_node, std::uint64_t page_count) noexcept
        : linux_node_{linux_node}, page_count_{page_count} {}

    core::LinuxNumaNodeId linux_node_;
    std::uint64_t page_count_;

    friend struct detail::PlacementObservedRegionAccess;
};

class RegionPlacementObservation {
  public:
    [[nodiscard]] auto total_pages() const noexcept -> std::uint64_t { return total_pages_; }
    [[nodiscard]] auto resident_pages() const noexcept -> std::uint64_t {
        return resident_pages_;
    }
    [[nodiscard]] auto unresolved_pages() const noexcept -> std::uint64_t {
        return unresolved_pages_;
    }
    [[nodiscard]] auto resident_page_counts() const noexcept
        -> std::span<const LinuxNodePageCount> {
        return resident_page_counts_;
    }

  private:
    RegionPlacementObservation(std::uint64_t total_pages, std::uint64_t resident_pages,
                               std::uint64_t unresolved_pages,
                               std::vector<LinuxNodePageCount> resident_page_counts) noexcept
        : total_pages_{total_pages}, resident_pages_{resident_pages},
          unresolved_pages_{unresolved_pages},
          resident_page_counts_{std::move(resident_page_counts)} {}

    std::uint64_t total_pages_;
    std::uint64_t resident_pages_;
    std::uint64_t unresolved_pages_;
    std::vector<LinuxNodePageCount> resident_page_counts_;

    friend struct detail::PlacementObservedRegionAccess;
};

enum class PreRunPlacementQueryState { skipped, observed };

class RegionPlacementQueryFailure {
  public:
    RegionPlacementQueryFailure(RegionPlacementQueryFailure&&) noexcept = default;
    auto operator=(RegionPlacementQueryFailure&&) noexcept
        -> RegionPlacementQueryFailure& = default;

    RegionPlacementQueryFailure(const RegionPlacementQueryFailure&) = delete;
    auto operator=(const RegionPlacementQueryFailure&)
        -> RegionPlacementQueryFailure& = delete;

    ~RegionPlacementQueryFailure() noexcept = default;

    [[nodiscard]] auto error() const noexcept -> const core::Error& { return error_; }
    [[nodiscard]] auto region() const noexcept -> const InitializedRegion& { return region_; }
    [[nodiscard]] auto take_region() && noexcept -> InitializedRegion {
        return std::move(region_);
    }

  private:
    RegionPlacementQueryFailure(core::Error error, InitializedRegion region) noexcept
        : error_{std::move(error)}, region_{std::move(region)} {}

    core::Error error_;
    InitializedRegion region_;

    friend struct detail::PlacementObservedRegionAccess;
};

class PlacementObservedRegion {
  public:
    [[nodiscard]] static auto create(InitializedRegion region) noexcept
        -> std::expected<PlacementObservedRegion, RegionPlacementQueryFailure>;

    PlacementObservedRegion(PlacementObservedRegion&&) noexcept = default;
    auto operator=(PlacementObservedRegion&&) noexcept
        -> PlacementObservedRegion& = default;

    PlacementObservedRegion(const PlacementObservedRegion&) = delete;
    auto operator=(const PlacementObservedRegion&) -> PlacementObservedRegion& = delete;

    ~PlacementObservedRegion() noexcept = default;

    [[nodiscard]] auto region() const noexcept -> const InitializedRegion& { return region_; }
    [[nodiscard]] auto request() const noexcept -> const RegionRequest& {
        return region_.request();
    }
    [[nodiscard]] auto query_state() const noexcept -> PreRunPlacementQueryState {
        return query_state_;
    }
    [[nodiscard]] auto observation() const noexcept
        -> const std::optional<RegionPlacementObservation>& {
        return observation_;
    }

  private:
    PlacementObservedRegion(InitializedRegion region, PreRunPlacementQueryState query_state,
                            std::optional<RegionPlacementObservation> observation) noexcept
        : region_{std::move(region)}, query_state_{query_state},
          observation_{std::move(observation)} {}

    InitializedRegion region_;
    PreRunPlacementQueryState query_state_;
    std::optional<RegionPlacementObservation> observation_;

    friend struct detail::PlacementObservedRegionAccess;
};

} // namespace epycbench::memory
