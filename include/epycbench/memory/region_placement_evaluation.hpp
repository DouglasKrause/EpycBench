#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/memory/region_placement_query.hpp"
#include "epycbench/memory/region_request.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct PlacementEvaluatedRegionAccess;
}

enum class PlacementQualificationState { skipped, qualified, not_qualified };

class RegionPlacementEvaluation {
  public:
    [[nodiscard]] auto requested_node_pages() const noexcept -> std::uint64_t {
        return requested_node_pages_;
    }
    [[nodiscard]] auto unexpected_node_pages() const noexcept -> std::uint64_t {
        return unexpected_node_pages_;
    }
    [[nodiscard]] auto unresolved_pages() const noexcept -> std::uint64_t {
        return unresolved_pages_;
    }
    [[nodiscard]] auto observed_ratio() const noexcept -> const ExactRatio& {
        return observed_ratio_;
    }

  private:
    RegionPlacementEvaluation(std::uint64_t requested_node_pages,
                              std::uint64_t unexpected_node_pages,
                              std::uint64_t unresolved_pages,
                              ExactRatio observed_ratio) noexcept
        : requested_node_pages_{requested_node_pages},
          unexpected_node_pages_{unexpected_node_pages},
          unresolved_pages_{unresolved_pages}, observed_ratio_{observed_ratio} {}

    std::uint64_t requested_node_pages_;
    std::uint64_t unexpected_node_pages_;
    std::uint64_t unresolved_pages_;
    ExactRatio observed_ratio_;

    friend struct detail::PlacementEvaluatedRegionAccess;
};

class RegionPlacementEvaluationFailure {
  public:
    RegionPlacementEvaluationFailure(RegionPlacementEvaluationFailure&&) noexcept = default;
    auto operator=(RegionPlacementEvaluationFailure&&) noexcept
        -> RegionPlacementEvaluationFailure& = default;

    RegionPlacementEvaluationFailure(const RegionPlacementEvaluationFailure&) = delete;
    auto operator=(const RegionPlacementEvaluationFailure&)
        -> RegionPlacementEvaluationFailure& = delete;

    ~RegionPlacementEvaluationFailure() noexcept = default;

    [[nodiscard]] auto error() const noexcept -> const core::Error& { return error_; }
    [[nodiscard]] auto region() const noexcept -> const PlacementObservedRegion& {
        return region_;
    }
    [[nodiscard]] auto take_region() && noexcept -> PlacementObservedRegion {
        return std::move(region_);
    }

  private:
    RegionPlacementEvaluationFailure(core::Error error,
                                     PlacementObservedRegion region) noexcept
        : error_{std::move(error)}, region_{std::move(region)} {}

    core::Error error_;
    PlacementObservedRegion region_;

    friend struct detail::PlacementEvaluatedRegionAccess;
};

class PlacementEvaluatedRegion {
  public:
    [[nodiscard]] static auto create(PlacementObservedRegion region) noexcept
        -> std::expected<PlacementEvaluatedRegion,
                         RegionPlacementEvaluationFailure>;

    PlacementEvaluatedRegion(PlacementEvaluatedRegion&&) noexcept = default;
    auto operator=(PlacementEvaluatedRegion&&) noexcept
        -> PlacementEvaluatedRegion& = default;

    PlacementEvaluatedRegion(const PlacementEvaluatedRegion&) = delete;
    auto operator=(const PlacementEvaluatedRegion&)
        -> PlacementEvaluatedRegion& = delete;

    ~PlacementEvaluatedRegion() noexcept = default;

    [[nodiscard]] auto region() const noexcept -> const PlacementObservedRegion& {
        return region_;
    }
    [[nodiscard]] auto request() const noexcept -> const RegionRequest& {
        return region_.request();
    }
    [[nodiscard]] auto qualification_state() const noexcept
        -> PlacementQualificationState {
        return qualification_state_;
    }
    [[nodiscard]] auto evaluation() const noexcept
        -> const std::optional<RegionPlacementEvaluation>& {
        return evaluation_;
    }
    [[nodiscard]] auto execution_eligible() const noexcept -> bool {
        return qualification_state_ != PlacementQualificationState::not_qualified;
    }

  private:
    PlacementEvaluatedRegion(
        PlacementObservedRegion region,
        PlacementQualificationState qualification_state,
        std::optional<RegionPlacementEvaluation> evaluation) noexcept
        : region_{std::move(region)}, qualification_state_{qualification_state},
          evaluation_{std::move(evaluation)} {}

    PlacementObservedRegion region_;
    PlacementQualificationState qualification_state_;
    std::optional<RegionPlacementEvaluation> evaluation_;

    friend struct detail::PlacementEvaluatedRegionAccess;
};

} // namespace epycbench::memory
