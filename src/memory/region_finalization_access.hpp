#pragma once

#include "epycbench/memory/region_finalization.hpp"
#include "memory/region_placement_query_access.hpp"

namespace epycbench::memory::detail {

struct RegionFinalizationAccess {
    [[nodiscard]] static auto take_diagnostic(FinalizedRegion&&) noexcept
        -> std::optional<RegionFinalizationDiagnostic>;
    [[nodiscard]] static auto take_failure_diagnostic(
        FinalizedRegion::Owner&&, std::optional<PostRunPlacementEvidence>&&,
        RegionCleanupEvidence, RegionFinalizationPath) noexcept
        -> RegionFinalizationDiagnostic;
    [[nodiscard]] static auto finalize_with_operations(
        ReadOnlyRegion region, PostRunVerificationApplicability applicability,
        const RegionPlacementQueryOperations& operations) noexcept
        -> std::expected<FinalizedRegion, RegionFinalizationFailure>;

    [[nodiscard]] static auto cleanup(bool succeeded) noexcept
        -> RegionCleanupEvidence { return RegionCleanupEvidence{true, succeeded, !succeeded}; }
    [[nodiscard]] static auto cleanup_not_attempted(bool active_after) noexcept
        -> RegionCleanupEvidence { return RegionCleanupEvidence{false, false, active_after}; }
    [[nodiscard]] static auto post_run(
        PostRunVerificationState state,
        std::optional<RegionPlacementObservation> observation = std::nullopt,
        std::optional<RegionPlacementEvaluation> evaluation = std::nullopt,
        std::optional<PlacementQualificationState> qualification = std::nullopt) noexcept
        -> PostRunPlacementEvidence {
        return {state, std::move(observation), std::move(evaluation), qualification};
    }
    [[nodiscard]] static auto success(
        FinalizedRegion::Owner owner, PostRunPlacementEvidence evidence,
        RegionCleanupEvidence cleanup_evidence, RegionFinalizationPath path) noexcept
        -> FinalizedRegion {
        return {std::move(owner), std::move(evidence), cleanup_evidence, path};
    }
    [[nodiscard]] static auto failure(
        RegionFinalizationFailure::Owner owner, core::Error primary,
        std::optional<core::Error> cleanup_error,
        std::optional<PostRunPlacementEvidence> evidence,
        RegionCleanupEvidence cleanup_evidence,
        RegionFinalizationPath path) noexcept -> RegionFinalizationFailure {
        return {std::move(owner), std::move(primary), std::move(cleanup_error),
                std::move(evidence), cleanup_evidence, path};
    }
    [[nodiscard]] static auto owner(FinalizedRegion& value) noexcept
        -> FinalizedRegion::Owner& { return value.owner_; }
    [[nodiscard]] static auto owner(RegionFinalizationFailure& value) noexcept
        -> RegionFinalizationFailure::Owner& { return value.owner_; }
    [[nodiscard]] static auto owner(const RegionFinalizationFailure& value) noexcept
        -> const RegionFinalizationFailure::Owner& { return value.owner_; }
    [[nodiscard]] static auto take_owner(FinalizedRegion& value) noexcept
        -> FinalizedRegion::Owner { return std::move(value.owner_); }
    [[nodiscard]] static auto take_post_run(FinalizedRegion& value) noexcept
        -> PostRunPlacementEvidence { return std::move(value.post_run_evidence_); }
    static void set_cleanup_failure(RegionFinalizationFailure& value,
                                    core::Error error) noexcept {
        value.cleanup_error_ = std::move(error);
        value.cleanup_evidence_ = cleanup(false);
    }
    [[nodiscard]] static auto take_primary_error(RegionFinalizationFailure& value) noexcept
        -> core::Error { return std::move(value.primary_error_); }
    [[nodiscard]] static auto take_cleanup_error(RegionFinalizationFailure& value) noexcept
        -> std::optional<core::Error> { return std::move(value.cleanup_error_); }
    [[nodiscard]] static auto take_post_run(RegionFinalizationFailure& value) noexcept
        -> std::optional<PostRunPlacementEvidence> {
        return std::move(value.post_run_evidence_);
    }
};

} // namespace epycbench::memory::detail
