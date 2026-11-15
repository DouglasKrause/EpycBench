#pragma once

#include "epycbench/memory/region_finalization.hpp"

#include <expected>
#include <optional>
#include <utility>

namespace epycbench::memory {

namespace detail {
struct RegionPlacementDiagnosticAccess;
struct RegionFinalizationDiagnosticTestAccess;
}

enum class PreRunDiagnosticState { skipped, observed };

class PreRunPlacementDiagnostic {
  public:
    [[nodiscard]] auto state() const noexcept -> PreRunDiagnosticState { return state_; }
    [[nodiscard]] auto observation() const noexcept
        -> const std::optional<RegionPlacementObservation>& { return observation_; }
    [[nodiscard]] auto evaluation() const noexcept
        -> const std::optional<RegionPlacementEvaluation>& { return evaluation_; }
    [[nodiscard]] auto qualification_state() const noexcept
        -> std::optional<PlacementQualificationState> { return qualification_; }

  private:
    PreRunPlacementDiagnostic(
        PreRunDiagnosticState state,
        std::optional<RegionPlacementObservation> observation,
        std::optional<RegionPlacementEvaluation> evaluation,
        std::optional<PlacementQualificationState> qualification) noexcept
        : state_{state}, observation_{std::move(observation)},
          evaluation_{std::move(evaluation)}, qualification_{qualification} {}

    PreRunDiagnosticState state_;
    std::optional<RegionPlacementObservation> observation_;
    std::optional<RegionPlacementEvaluation> evaluation_;
    std::optional<PlacementQualificationState> qualification_;
    friend struct detail::RegionPlacementDiagnosticAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class RegionPlacementDiagnostic {
  public:
    [[nodiscard]] static auto create(const FinalizedRegion& region) noexcept
        -> std::expected<RegionPlacementDiagnostic, core::Error>;

    RegionPlacementDiagnostic(const RegionPlacementDiagnostic&) = default;
    auto operator=(const RegionPlacementDiagnostic&)
        -> RegionPlacementDiagnostic& = default;
    RegionPlacementDiagnostic(RegionPlacementDiagnostic&&) noexcept = default;
    auto operator=(RegionPlacementDiagnostic&&) noexcept
        -> RegionPlacementDiagnostic& = default;
    ~RegionPlacementDiagnostic() noexcept = default;

    [[nodiscard]] auto request() const noexcept -> const RegionRequest& { return request_; }
    [[nodiscard]] auto binding_evidence() const noexcept
        -> const RegionBindingEvidence& { return binding_evidence_; }
    [[nodiscard]] auto initialization_evidence() const noexcept
        -> const RegionInitializationEvidence& { return initialization_evidence_; }
    [[nodiscard]] auto pre_run() const noexcept
        -> const PreRunPlacementDiagnostic& { return pre_run_; }
    [[nodiscard]] auto protection_evidence() const noexcept
        -> const std::optional<RegionProtectionEvidence>& { return protection_evidence_; }
    [[nodiscard]] auto post_run() const noexcept
        -> const PostRunPlacementEvidence& { return post_run_; }
    [[nodiscard]] auto cleanup_evidence() const noexcept
        -> const RegionCleanupEvidence& { return cleanup_evidence_; }
    [[nodiscard]] auto finalization_path() const noexcept -> RegionFinalizationPath {
        return path_;
    }
    [[nodiscard]] auto runtime_placement_projection_eligible() const noexcept -> bool {
        return path_ == RegionFinalizationPath::protected_execution;
    }

  private:
    RegionPlacementDiagnostic(
        RegionRequest request, RegionBindingEvidence binding_evidence,
        RegionInitializationEvidence initialization_evidence,
        PreRunPlacementDiagnostic pre_run,
        std::optional<RegionProtectionEvidence> protection_evidence,
        PostRunPlacementEvidence post_run, RegionCleanupEvidence cleanup_evidence,
        RegionFinalizationPath path) noexcept
        : request_{std::move(request)}, binding_evidence_{binding_evidence},
          initialization_evidence_{initialization_evidence}, pre_run_{std::move(pre_run)},
          protection_evidence_{std::move(protection_evidence)},
          post_run_{std::move(post_run)}, cleanup_evidence_{cleanup_evidence}, path_{path} {}

    RegionRequest request_;
    RegionBindingEvidence binding_evidence_;
    RegionInitializationEvidence initialization_evidence_;
    PreRunPlacementDiagnostic pre_run_;
    std::optional<RegionProtectionEvidence> protection_evidence_;
    PostRunPlacementEvidence post_run_;
    RegionCleanupEvidence cleanup_evidence_;
    RegionFinalizationPath path_;
    friend struct detail::RegionPlacementDiagnosticAccess;
};

} // namespace epycbench::memory
