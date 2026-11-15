#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/memory/region_protection.hpp"

#include <expected>
#include <optional>
#include <utility>
#include <variant>

namespace epycbench::memory {

namespace detail {
struct RegionFinalizationAccess;
struct RetainedRegionCleanupAccess;
struct RegionFinalizationDiagnosticTestAccess;
}

enum class PostRunVerificationApplicability { not_applicable, applicable };
enum class PostRunVerificationState { not_applicable, skipped, observed };
enum class UnprotectedFinalizationReason { execution_ineligible, protection_failed };
enum class RegionFinalizationPath {
    protected_execution,
    unprotected_execution_ineligible,
    unprotected_protection_failed,
};
enum class RegionCleanupRange { full_mapping };

class RegionCleanupEvidence {
  public:
    [[nodiscard]] auto release_attempted() const noexcept -> bool { return attempted_; }
    [[nodiscard]] auto release_succeeded() const noexcept -> bool { return succeeded_; }
    [[nodiscard]] auto range() const noexcept -> RegionCleanupRange {
        return RegionCleanupRange::full_mapping;
    }
    [[nodiscard]] auto mapping_active_after() const noexcept -> bool {
        return active_after_;
    }

  private:
    RegionCleanupEvidence(bool attempted, bool succeeded, bool active_after) noexcept
        : attempted_{attempted}, succeeded_{succeeded}, active_after_{active_after} {}
    bool attempted_;
    bool succeeded_;
    bool active_after_;
    friend struct detail::RegionFinalizationAccess;
    friend struct detail::RetainedRegionCleanupAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class PostRunPlacementEvidence {
  public:
    [[nodiscard]] auto state() const noexcept -> PostRunVerificationState { return state_; }
    [[nodiscard]] auto observation() const noexcept
        -> const std::optional<RegionPlacementObservation>& { return observation_; }
    [[nodiscard]] auto evaluation() const noexcept
        -> const std::optional<RegionPlacementEvaluation>& { return evaluation_; }
    [[nodiscard]] auto qualification_state() const noexcept
        -> std::optional<PlacementQualificationState> { return qualification_state_; }

  private:
    PostRunPlacementEvidence(
        PostRunVerificationState state,
        std::optional<RegionPlacementObservation> observation,
        std::optional<RegionPlacementEvaluation> evaluation,
        std::optional<PlacementQualificationState> qualification_state) noexcept
        : state_{state}, observation_{std::move(observation)},
          evaluation_{std::move(evaluation)}, qualification_state_{qualification_state} {}

    PostRunVerificationState state_;
    std::optional<RegionPlacementObservation> observation_;
    std::optional<RegionPlacementEvaluation> evaluation_;
    std::optional<PlacementQualificationState> qualification_state_;
    friend struct detail::RegionFinalizationAccess;
    friend struct detail::RegionFinalizationDiagnosticTestAccess;
};

class RegionFinalizationFailure;
class RegionFinalizationDiagnostic;

class FinalizedRegion {
  public:
    [[nodiscard]] static auto finalize(
        ReadOnlyRegion region,
        PostRunVerificationApplicability applicability) noexcept
        -> std::expected<FinalizedRegion, RegionFinalizationFailure>;

    [[nodiscard]] static auto finalize_unprotected(
        PlacementEvaluatedRegion region,
        UnprotectedFinalizationReason reason) noexcept
        -> std::expected<FinalizedRegion, RegionFinalizationFailure>;

    FinalizedRegion(FinalizedRegion&& other) noexcept
        : owner_{std::move(other.owner_)},
          post_run_evidence_{std::move(other.post_run_evidence_)},
          cleanup_evidence_{other.cleanup_evidence_}, path_{other.path_},
          diagnostic_taken_{std::exchange(other.diagnostic_taken_, true)} {}
    auto operator=(FinalizedRegion&& other) noexcept -> FinalizedRegion& {
        if (this != &other) {
            owner_ = std::move(other.owner_);
            post_run_evidence_ = std::move(other.post_run_evidence_);
            cleanup_evidence_ = other.cleanup_evidence_;
            path_ = other.path_;
            diagnostic_taken_ = std::exchange(other.diagnostic_taken_, true);
        }
        return *this;
    }
    FinalizedRegion(const FinalizedRegion&) = delete;
    auto operator=(const FinalizedRegion&) -> FinalizedRegion& = delete;
    ~FinalizedRegion() noexcept = default;

    [[nodiscard]] auto request() const noexcept -> const RegionRequest&;
    [[nodiscard]] auto evaluated_region() const noexcept
        -> const PlacementEvaluatedRegion&;
    [[nodiscard]] auto protection_evidence() const noexcept
        -> std::optional<RegionProtectionEvidence>;
    [[nodiscard]] auto post_run_evidence() const noexcept
        -> const PostRunPlacementEvidence& { return post_run_evidence_; }
    [[nodiscard]] auto cleanup_evidence() const noexcept
        -> const RegionCleanupEvidence& { return cleanup_evidence_; }
    [[nodiscard]] auto finalization_path() const noexcept -> RegionFinalizationPath {
        return path_;
    }
    [[nodiscard]] auto diagnostic() const noexcept -> RegionFinalizationDiagnostic;

  private:
    using Owner = std::variant<ReadOnlyRegion, PlacementEvaluatedRegion>;
    FinalizedRegion(Owner owner, PostRunPlacementEvidence post_run_evidence,
                    RegionCleanupEvidence cleanup_evidence,
                    RegionFinalizationPath path) noexcept
        : owner_{std::move(owner)}, post_run_evidence_{std::move(post_run_evidence)},
          cleanup_evidence_{cleanup_evidence}, path_{path} {}

    Owner owner_;
    PostRunPlacementEvidence post_run_evidence_;
    RegionCleanupEvidence cleanup_evidence_;
    RegionFinalizationPath path_;
    bool diagnostic_taken_{};
    friend struct detail::RegionFinalizationAccess;
};

class RegionFinalizationFailure {
  public:
    RegionFinalizationFailure(RegionFinalizationFailure&&) noexcept = default;
    auto operator=(RegionFinalizationFailure&&) noexcept
        -> RegionFinalizationFailure& = default;
    RegionFinalizationFailure(const RegionFinalizationFailure&) = delete;
    auto operator=(const RegionFinalizationFailure&)
        -> RegionFinalizationFailure& = delete;
    ~RegionFinalizationFailure() noexcept = default;

    [[nodiscard]] auto primary_error() const noexcept -> const core::Error& {
        return primary_error_;
    }
    [[nodiscard]] auto cleanup_error() const noexcept
        -> const std::optional<core::Error>& { return cleanup_error_; }
    [[nodiscard]] auto cleanup_evidence() const noexcept
        -> const RegionCleanupEvidence& { return cleanup_evidence_; }
    [[nodiscard]] auto finalization_path() const noexcept -> RegionFinalizationPath {
        return path_;
    }
    [[nodiscard]] auto diagnostic() const noexcept -> RegionFinalizationDiagnostic;
    [[nodiscard]] auto evaluated_region() const noexcept
        -> const PlacementEvaluatedRegion&;
    [[nodiscard]] auto post_run_evidence() const noexcept
        -> const std::optional<PostRunPlacementEvidence>& { return post_run_evidence_; }
    [[nodiscard]] auto recover_protected_region() && noexcept
        -> std::optional<ReadOnlyRegion>;
    [[nodiscard]] auto recover_unprotected_region() && noexcept
        -> std::optional<PlacementEvaluatedRegion>;

  private:
    using Owner = std::variant<ReadOnlyRegion, PlacementEvaluatedRegion>;
    RegionFinalizationFailure(Owner owner, core::Error primary_error,
                              std::optional<core::Error> cleanup_error,
                              std::optional<PostRunPlacementEvidence> post_run_evidence,
                              RegionCleanupEvidence cleanup_evidence,
                              RegionFinalizationPath path) noexcept
        : owner_{std::move(owner)}, primary_error_{std::move(primary_error)},
          cleanup_error_{std::move(cleanup_error)},
          post_run_evidence_{std::move(post_run_evidence)},
          cleanup_evidence_{cleanup_evidence}, path_{path} {}

    Owner owner_;
    core::Error primary_error_;
    std::optional<core::Error> cleanup_error_;
    std::optional<PostRunPlacementEvidence> post_run_evidence_;
    RegionCleanupEvidence cleanup_evidence_;
    RegionFinalizationPath path_;
    friend struct detail::RegionFinalizationAccess;
};

} // namespace epycbench::memory
