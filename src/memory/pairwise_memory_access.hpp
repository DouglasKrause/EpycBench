#pragma once

#include "epycbench/memory/region_request.hpp"
#include "memory/region_binding_access.hpp"
#include "memory/region_finalization_access.hpp"
#include "memory/region_initialization_access.hpp"
#include "memory/region_mapping_access.hpp"
#include "memory/region_placement_evaluation_access.hpp"
#include "memory/region_placement_query_access.hpp"
#include "memory/region_protection_access.hpp"
#include "memory/placement_diagnostic_access.hpp"
#include "memory/region_finalization_diagnostic_access.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace epycbench::memory::detail {

struct PairwiseMemoryAccess final {
    [[nodiscard]] static auto numa_target_capacity(
        const RegionRequest& request) noexcept -> std::size_t {
        return request.numa_targets_.capacity();
    }
    [[nodiscard]] static auto ratio(std::uint64_t numerator,
                                    std::uint64_t denominator) noexcept -> ExactRatio {
        return ExactRatio{numerator, denominator};
    }
    [[nodiscard]] static auto request(RegionRequestInput input) noexcept -> RegionRequest {
        return RegionRequest{std::move(input)};
    }
    [[nodiscard]] static auto binding_evidence() noexcept -> RegionBindingEvidence {
        return BoundRegionAccess::evidence();
    }
    [[nodiscard]] static auto initialization_evidence(bool prefault) noexcept
        -> RegionInitializationEvidence {
        return InitializedRegionAccess::evidence(prefault);
    }
    [[nodiscard]] static auto protection_evidence() noexcept -> RegionProtectionEvidence {
        return ReadOnlyRegionAccess::evidence();
    }
    [[nodiscard]] static auto placement_evaluation(
        std::uint64_t requested, std::uint64_t unexpected, std::uint64_t unresolved,
        ExactRatio ratio) noexcept -> RegionPlacementEvaluation {
        return PlacementEvaluatedRegionAccess::evaluation(
            requested, unexpected, unresolved, ratio);
    }
    [[nodiscard]] static auto pre_run(
        PreRunDiagnosticState state,
        std::optional<RegionPlacementObservation> observation,
        std::optional<RegionPlacementEvaluation> evaluation,
        std::optional<PlacementQualificationState> qualification) noexcept
        -> PreRunPlacementDiagnostic {
        return RegionPlacementDiagnosticAccess::pre_run(
            state, std::move(observation), std::move(evaluation), qualification);
    }
    [[nodiscard]] static auto post_run(
        PostRunVerificationState state,
        std::optional<RegionPlacementObservation> observation,
        std::optional<RegionPlacementEvaluation> evaluation,
        std::optional<PlacementQualificationState> qualification) noexcept
        -> PostRunPlacementEvidence {
        return RegionFinalizationAccess::post_run(
            state, std::move(observation), std::move(evaluation), qualification);
    }
    [[nodiscard]] static auto cleanup(bool attempted, bool succeeded,
                                      bool active_after) noexcept
        -> RegionCleanupEvidence {
        return attempted ? RegionFinalizationAccess::cleanup(succeeded)
                         : RegionFinalizationAccess::cleanup_not_attempted(active_after);
    }
    [[nodiscard]] static auto diagnostic(
        RegionRequest request, RegionBindingEvidence binding,
        RegionInitializationEvidence initialization, PreRunPlacementDiagnostic pre,
        std::optional<RegionProtectionEvidence> protection,
        std::optional<PostRunPlacementEvidence> post, RegionCleanupEvidence cleanup,
        RegionFinalizationPath path, RegionFinalizationDisposition disposition) noexcept
        -> RegionFinalizationDiagnostic {
        return RegionFinalizationDiagnosticAccess::available(
            std::move(request), binding, initialization, std::move(pre),
            std::move(protection), std::move(post), cleanup, path, disposition);
    }
    [[nodiscard]] static auto unavailable_diagnostic(
        RegionFinalizationUnavailableReason reason) noexcept
        -> RegionFinalizationDiagnostic {
        return RegionFinalizationDiagnosticAccess::unavailable(reason);
    }
    [[nodiscard]] static auto mapping(RegionRequest request,
                                      platform_linux::AnonymousMapping mapping,
                                      std::size_t bytes) noexcept -> RegionMapping {
        return RegionMappingAccess::success(std::move(request), std::move(mapping), bytes);
    }
    [[nodiscard]] static auto bound(RegionMapping mapping) noexcept -> BoundRegion {
        return BoundRegionAccess::success(std::move(mapping), BoundRegionAccess::evidence());
    }
    [[nodiscard]] static auto initialized(BoundRegion region,
                                          bool prefault) noexcept -> InitializedRegion {
        return InitializedRegionAccess::success(
            std::move(region), InitializedRegionAccess::evidence(prefault));
    }
    [[nodiscard]] static auto node_count(core::LinuxNumaNodeId node,
                                         std::uint64_t count) noexcept
        -> LinuxNodePageCount {
        return PlacementObservedRegionAccess::node_count(node, count);
    }
    [[nodiscard]] static auto observation(
        std::uint64_t total, std::uint64_t resident, std::uint64_t unresolved,
        std::vector<LinuxNodePageCount> counts) noexcept -> RegionPlacementObservation {
        return PlacementObservedRegionAccess::observation(
            total, resident, unresolved, std::move(counts));
    }
    [[nodiscard]] static auto observed(
        InitializedRegion region, PreRunPlacementQueryState state,
        std::optional<RegionPlacementObservation> observation) noexcept
        -> PlacementObservedRegion {
        return PlacementObservedRegionAccess::success(
            std::move(region), state, std::move(observation));
    }
    [[nodiscard]] static auto evaluated(
        PlacementObservedRegion region, PlacementQualificationState state,
        std::optional<RegionPlacementEvaluation> evaluation) noexcept
        -> PlacementEvaluatedRegion {
        return PlacementEvaluatedRegionAccess::success(
            std::move(region), state, std::move(evaluation));
    }
    [[nodiscard]] static auto read_only(PlacementEvaluatedRegion region) noexcept
        -> ReadOnlyRegion {
        return ReadOnlyRegionAccess::success(std::move(region), ReadOnlyRegionAccess::evidence());
    }
    [[nodiscard]] static auto native_mapping(RegionMapping& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return RegionMappingAccess::mapping(region);
    }
    [[nodiscard]] static auto native_mapping(BoundRegion& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return RegionMappingAccess::mapping(BoundRegionAccess::mapping(region));
    }
    [[nodiscard]] static auto native_mapping(InitializedRegion& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return native_mapping(InitializedRegionAccess::region(region));
    }
    [[nodiscard]] static auto native_mapping(PlacementObservedRegion& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return native_mapping(PlacementObservedRegionAccess::region(region));
    }
    [[nodiscard]] static auto native_mapping(PlacementEvaluatedRegion& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return native_mapping(PlacementEvaluatedRegionAccess::region(region));
    }
    [[nodiscard]] static auto native_mapping(ReadOnlyRegion& region) noexcept
        -> platform_linux::AnonymousMapping& {
        return native_mapping(ReadOnlyRegionAccess::region(region));
    }
};

} // namespace epycbench::memory::detail
