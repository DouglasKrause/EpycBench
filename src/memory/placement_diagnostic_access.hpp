#pragma once

#include "epycbench/memory/placement_diagnostic.hpp"

namespace epycbench::memory::detail {

struct RegionPlacementDiagnosticAccess {
    [[nodiscard]] static auto pre_run(
        PreRunDiagnosticState state,
        std::optional<RegionPlacementObservation> observation,
        std::optional<RegionPlacementEvaluation> evaluation,
        std::optional<PlacementQualificationState> qualification) noexcept
        -> PreRunPlacementDiagnostic {
        return {state, std::move(observation), std::move(evaluation), qualification};
    }

    [[nodiscard]] static auto diagnostic(
        RegionRequest request, RegionBindingEvidence binding_evidence,
        RegionInitializationEvidence initialization_evidence,
        PreRunPlacementDiagnostic pre_run,
        std::optional<RegionProtectionEvidence> protection_evidence,
        PostRunPlacementEvidence post_run, RegionCleanupEvidence cleanup_evidence,
        RegionFinalizationPath path) noexcept -> RegionPlacementDiagnostic {
        return {std::move(request), binding_evidence, initialization_evidence,
                std::move(pre_run), std::move(protection_evidence), std::move(post_run),
                cleanup_evidence, path};
    }
};

} // namespace epycbench::memory::detail
