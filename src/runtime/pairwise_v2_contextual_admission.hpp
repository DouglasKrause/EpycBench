#pragma once

#include "pairwise_v2_contextual_admission_classification.hpp"
#include "runtime/pairwise_monitoring_host_capacity_startup.hpp"

#include <expected>
#include <memory>

namespace epycbench::runtime::detail {

[[nodiscard]] auto pairwise_v2_contextual_rejection_category(
    PairwiseV2ContextualAdmissionBranch) noexcept
    -> PairwiseHostCapacityRejectionReason;

enum class PairwiseV2ContextualAdmissionStage : unsigned char {
    owner_preparation,
    first_placement,
    second_placement,
    pair_projection
};

struct PairwiseV2ContextualAdmissionOperations final {
    void* context{};
    void (*before_stage)(void*, PairwiseV2ContextualAdmissionStage){};
};

class PairwiseV2ContextualAdmissionFailure final {
  public:
    PairwiseV2ContextualAdmissionFailure(const PairwiseV2ContextualAdmissionFailure&) = delete;
    PairwiseV2ContextualAdmissionFailure(PairwiseV2ContextualAdmissionFailure&& other) noexcept;
    auto operator=(const PairwiseV2ContextualAdmissionFailure&)
        -> PairwiseV2ContextualAdmissionFailure& = delete;
    auto operator=(PairwiseV2ContextualAdmissionFailure&&)
        -> PairwiseV2ContextualAdmissionFailure& = delete;

  private:
    PairwiseV2ContextualAdmissionFailure(
        ValidatedRemoteCapacityCommitment proof,
        PairwiseV2ContextualAdmissionStage stage,
        PairwiseV2ContextualAdmissionBranch branch) noexcept
        : proof_{std::move(proof)}, stage_{stage}, branch_{branch}, active_{true} {}

    ValidatedRemoteCapacityCommitment proof_;
    PairwiseV2ContextualAdmissionStage stage_{};
    PairwiseV2ContextualAdmissionBranch branch_{};
    bool active_{};

    friend struct PairwiseV2ContextualAdmissionAccess;
};

class PairwiseV2ContextualAdmission final {
  public:
    PairwiseV2ContextualAdmission(const PairwiseV2ContextualAdmission&) = delete;
    PairwiseV2ContextualAdmission(PairwiseV2ContextualAdmission&&) noexcept;
    ~PairwiseV2ContextualAdmission();
    auto operator=(const PairwiseV2ContextualAdmission&)
        -> PairwiseV2ContextualAdmission& = delete;
    auto operator=(PairwiseV2ContextualAdmission&&)
        -> PairwiseV2ContextualAdmission& = delete;

  private:
    struct State;
    explicit PairwiseV2ContextualAdmission(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;

    friend struct PairwiseV2ContextualAdmissionAccess;
    friend class PairwiseV2AdmittedStartup;
};

// Admitted placements borrow their request and context. Keep the original
// contextual owner at its stable address instead of moving its value members.
class PairwiseV2AdmittedStartup final {
  public:
    PairwiseV2AdmittedStartup(const PairwiseV2AdmittedStartup&) = delete;
    PairwiseV2AdmittedStartup(PairwiseV2AdmittedStartup&&) noexcept = default;
    auto operator=(const PairwiseV2AdmittedStartup&) -> PairwiseV2AdmittedStartup& = delete;
    auto operator=(PairwiseV2AdmittedStartup&&) -> PairwiseV2AdmittedStartup& = delete;
    [[nodiscard]] auto request() const noexcept -> const PairwiseHostCapacityRequestTranscript&;
    [[nodiscard]] auto matches_request(ipc::RequestId first, ipc::RequestId second,
                                       const core::Sha256Digest& digest) const noexcept -> bool;
    [[nodiscard]] auto inputs() const noexcept -> const AdmittedPairwiseProductionInputs&;
    [[nodiscard]] auto startup() noexcept -> AdmittedPairwiseHostCapacityStartup& { return startup_; }

  private:
    PairwiseV2AdmittedStartup(PairwiseV2ContextualAdmission owner,
                              AdmittedPairwiseHostCapacityStartup startup) noexcept
        : owner_{std::move(owner)}, startup_{std::move(startup)} {}
    PairwiseV2ContextualAdmission owner_;
    AdmittedPairwiseHostCapacityStartup startup_;
    friend struct PairwiseV2ContextualAdmissionAccess;
};

struct PairwiseV2ContextualAdmissionAccess final {
    [[nodiscard]] static auto admit_after_gate_a(
        PairwiseHostCapacityRequestTranscript transcript, RunnerContext context,
        ValidatedRemoteCapacityCommitment proof,
        PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority runner) noexcept
        -> std::expected<PairwiseV2ContextualAdmission,
                         PairwiseV2ContextualAdmissionFailure>;
    [[nodiscard]] static auto admit_after_gate_a_with(
        PairwiseHostCapacityRequestTranscript transcript, RunnerContext context,
        ValidatedRemoteCapacityCommitment proof,
        PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority runner,
        PairwiseV2ContextualAdmissionOperations operations) noexcept
        -> std::expected<PairwiseV2ContextualAdmission,
                         PairwiseV2ContextualAdmissionFailure>;
    [[nodiscard]] static auto reject(
        PairwiseV2ContextualAdmissionFailure failure) noexcept
        -> HostCapacityRejectedAckCapability;
    [[nodiscard]] static auto failure_stage(
        const PairwiseV2ContextualAdmissionFailure& failure) noexcept
        -> PairwiseV2ContextualAdmissionStage { return failure.stage_; }
    [[nodiscard]] static auto failure_branch(
        const PairwiseV2ContextualAdmissionFailure& failure) noexcept
        -> PairwiseV2ContextualAdmissionBranch { return failure.branch_; }
    // Retains the exact contextually admitted pair for independent resource
    // capacity derivation. No second admission or caller-supplied pair is used.
    [[nodiscard]] static auto gate_b_with_inputs(PairwiseV2ContextualAdmission admission) noexcept
        -> std::expected<PairwiseV2AdmittedStartup, HostCapacityRejectedAckCapability>;
    [[nodiscard]] static auto gate_b(PairwiseV2ContextualAdmission admission) noexcept
        -> std::expected<AdmittedPairwiseHostCapacityStartup,
                         HostCapacityRejectedAckCapability>;
};

} // namespace epycbench::runtime::detail
