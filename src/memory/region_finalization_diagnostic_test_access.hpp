#pragma once
#include "epycbench/memory/region_finalization_diagnostic.hpp"
namespace epycbench::memory::detail {
struct RegionFinalizationDiagnosticTestAccess final {
    static auto available(core::WorkerId worker_id, bool succeeded = true,
                          bool cleanup_failed = false) {
        auto stable = topology::StableNumaNodeId::parse("numa/0");
        auto threshold = ExactRatio::create(19, 20);
        if (!stable || !threshold) std::terminate();
        auto request = RegionRequest::create({
            worker_id, core::ByteCount::from_base_units(4096),
            core::ByteCount::from_base_units(4096), PlacementPolicy::bind, PageMode::base,
            {{*stable, core::LinuxNumaNodeId{0}, core::NumaNodeId{0}}}, *threshold,
            true, true, true});
        if (!request) std::terminate();
        AvailableRegionFinalizationDiagnostic value{
            std::move(*request), RegionBindingEvidence{}, RegionInitializationEvidence{true},
            PreRunPlacementDiagnostic{PreRunDiagnosticState::observed, std::nullopt,
                                      std::nullopt, PlacementQualificationState::qualified},
            RegionProtectionEvidence{},
            PostRunPlacementEvidence{PostRunVerificationState::observed, std::nullopt,
                                     std::nullopt, PlacementQualificationState::qualified},
            RegionCleanupEvidence{true, !cleanup_failed, cleanup_failed},
            RegionFinalizationPath::protected_execution,
            succeeded ? RegionFinalizationDisposition::succeeded
                      : RegionFinalizationDisposition::failed};
        return RegionFinalizationDiagnostic{
            RegionFinalizationDiagnostic::Value{std::move(value)}};
    }
    static auto capture(const FinalizedRegion& value, bool unknown) noexcept {
        return RegionFinalizationDiagnostic::capture_with(
            value, unknown ? RegionFinalizationDiagnostic::Fault::unknown
                           : RegionFinalizationDiagnostic::Fault::allocation);
    }
    static auto capture(const RegionFinalizationFailure& value, bool unknown) noexcept {
        return RegionFinalizationDiagnostic::capture_with(
            value, unknown ? RegionFinalizationDiagnostic::Fault::unknown
                           : RegionFinalizationDiagnostic::Fault::allocation);
    }
};
}
