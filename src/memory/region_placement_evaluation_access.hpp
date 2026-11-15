#pragma once

#include "epycbench/memory/region_placement_evaluation.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace epycbench::memory::detail {

enum class ExactRatioOrdering { less, equal, greater };

struct PlacementEvaluationResult {
    RegionPlacementEvaluation evaluation;
    PlacementQualificationState qualification_state;
};

struct PlacementEvaluatedRegionAccess {
    [[nodiscard]] static auto take_evaluation(
        PlacementEvaluatedRegion& value) noexcept
        -> std::optional<RegionPlacementEvaluation> {
        return std::move(value.evaluation_);
    }
    [[nodiscard]] static auto region(PlacementEvaluatedRegion& value) noexcept
        -> PlacementObservedRegion& {
        return value.region_;
    }
    [[nodiscard]] static auto region(const PlacementEvaluatedRegion& value) noexcept
        -> const PlacementObservedRegion& {
        return value.region_;
    }
    [[nodiscard]] static auto execution_eligible(const PlacementEvaluatedRegion& value) noexcept
        -> bool {
        return value.execution_eligible();
    }

    [[nodiscard]] static auto compare_exact_ratios(const ExactRatio& left,
                                                   const ExactRatio& right) noexcept
        -> ExactRatioOrdering;

    [[nodiscard]] static auto
    evaluate_region_placement(const RegionRequest& request,
                              const RegionPlacementObservation& observation) noexcept
        -> std::expected<PlacementEvaluationResult, core::Error>;

    [[nodiscard]] static auto
    evaluation(std::uint64_t requested_node_pages, std::uint64_t unexpected_node_pages,
               std::uint64_t unresolved_pages, ExactRatio observed_ratio) noexcept
        -> RegionPlacementEvaluation {
        return {requested_node_pages, unexpected_node_pages, unresolved_pages, observed_ratio};
    }
    [[nodiscard]] static auto success(PlacementObservedRegion region,
                                      PlacementQualificationState qualification_state,
                                      std::optional<RegionPlacementEvaluation> evaluation) noexcept
        -> PlacementEvaluatedRegion {
        return {std::move(region), qualification_state, std::move(evaluation)};
    }
    [[nodiscard]] static auto failure(core::Error error, PlacementObservedRegion region) noexcept
        -> RegionPlacementEvaluationFailure {
        return {std::move(error), std::move(region)};
    }
};

} // namespace epycbench::memory::detail
