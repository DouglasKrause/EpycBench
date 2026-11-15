#pragma once
#include "epycbench/memory/region_finalization_diagnostic.hpp"
namespace epycbench::memory::detail {
struct RegionFinalizationDiagnosticAccess final {
    static auto available(
        RegionRequest request, RegionBindingEvidence binding,
        RegionInitializationEvidence initialization, PreRunPlacementDiagnostic pre_run,
        std::optional<RegionProtectionEvidence> protection,
        std::optional<PostRunPlacementEvidence> post_run, RegionCleanupEvidence cleanup,
        RegionFinalizationPath path, RegionFinalizationDisposition disposition) noexcept
        -> RegionFinalizationDiagnostic {
        return RegionFinalizationDiagnostic{
            RegionFinalizationDiagnostic::Value{AvailableRegionFinalizationDiagnostic{
                std::move(request), binding, initialization, std::move(pre_run),
                std::move(protection), std::move(post_run), cleanup, path, disposition}}};
    }
    static auto unavailable(RegionFinalizationUnavailableReason reason) noexcept
        -> RegionFinalizationDiagnostic {
        UnavailableRegionFinalizationDiagnostic unavailable{reason};
        return RegionFinalizationDiagnostic{
            RegionFinalizationDiagnostic::Value{std::move(unavailable)}};
    }
};
}
