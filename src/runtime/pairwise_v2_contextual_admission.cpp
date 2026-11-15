#include "runtime/pairwise_v2_contextual_admission.hpp"

#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace epycbench::runtime::detail {
static_assert(std::is_nothrow_move_constructible_v<AdmittedPairwiseProductionInputs>);

[[nodiscard]] auto pairwise_v2_contextual_rejection_category(
    PairwiseV2ContextualAdmissionBranch branch) noexcept
    -> PairwiseHostCapacityRejectionReason {
    using Branch = PairwiseV2ContextualAdmissionBranch;
    using Rejection = PairwiseHostCapacityRejectionReason;
    switch (branch) {
    case Branch::placement_context_invariant:
    case Branch::placement_topology_mismatch:
    case Branch::pair_topology_mismatch:
        return Rejection::topology_mismatch;
    case Branch::placement_present_cpu_mismatch:
        return Rejection::present_cpu_mismatch;
    case Branch::pair_provenance_mismatch:
        return Rejection::provenance_mismatch;
    case Branch::placement_policy_mismatch:
    case Branch::pair_context_mismatch:
    case Branch::pair_transport_mismatch:
    case Branch::pair_policy_mismatch:
    case Branch::first_monitoring_policy_mismatch:
    case Branch::second_monitoring_policy_mismatch:
    case Branch::monitoring_policy_mismatch:
    case Branch::service_placement_mismatch:
    case Branch::statistics_policy_mismatch:
        return Rejection::pairwise_policy_mismatch;
    case Branch::allocation:
        return Rejection::allocation;
    case Branch::placement_identity_failure:
    case Branch::placement_contained_exception:
    case Branch::service_placement_contained_exception:
    case Branch::statistics_contained_exception:
    case Branch::production_contained_exception:
        return Rejection::contained_exception;
    case Branch::placement_contained_unknown_failure:
    case Branch::service_placement_contained_unknown_failure:
    case Branch::statistics_contained_unknown_failure:
    case Branch::production_contained_unknown_failure:
        return Rejection::contained_unknown_failure;
    }
    std::terminate();
}

struct PairwiseV2ContextualAdmission::State final {
    State(RunnerContext runner_context, PairwiseHostCapacityRequestTranscript request,
          ValidatedRemoteCapacityCommitment validated,
          PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority lower) noexcept
        : context{std::move(runner_context)}, transcript{std::move(request)},
          proof{std::move(validated)}, runner{std::move(lower)} {}

    RunnerContext context;
    PairwiseHostCapacityRequestTranscript transcript;
    std::optional<AdmittedPairwiseProductionInputs> inputs;
    std::optional<ValidatedRemoteCapacityCommitment> proof;
    std::optional<PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority> runner;
};

PairwiseV2ContextualAdmission::PairwiseV2ContextualAdmission(
    std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

PairwiseV2ContextualAdmission::PairwiseV2ContextualAdmission(
    PairwiseV2ContextualAdmission&&) noexcept = default;

PairwiseV2ContextualAdmission::~PairwiseV2ContextualAdmission() = default;

PairwiseV2ContextualAdmissionFailure::PairwiseV2ContextualAdmissionFailure(
    PairwiseV2ContextualAdmissionFailure&& other) noexcept
    : proof_{std::move(other.proof_)}, stage_{other.stage_}, branch_{other.branch_},
      active_{std::exchange(other.active_, false)} {}

auto PairwiseV2ContextualAdmissionAccess::admit_after_gate_a(
    PairwiseHostCapacityRequestTranscript transcript, RunnerContext context,
    ValidatedRemoteCapacityCommitment proof,
    PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority runner) noexcept
    -> std::expected<PairwiseV2ContextualAdmission,
                     PairwiseV2ContextualAdmissionFailure> {
    return admit_after_gate_a_with(std::move(transcript), std::move(context),
                                   std::move(proof), std::move(runner), {});
}

auto PairwiseV2ContextualAdmissionAccess::admit_after_gate_a_with(
    PairwiseHostCapacityRequestTranscript transcript, RunnerContext context,
    ValidatedRemoteCapacityCommitment proof,
    PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority runner,
    PairwiseV2ContextualAdmissionOperations operations) noexcept
    -> std::expected<PairwiseV2ContextualAdmission,
                     PairwiseV2ContextualAdmissionFailure> {
    std::unique_ptr<PairwiseV2ContextualAdmission::State> state;
    try {
        if (operations.before_stage)
            operations.before_stage(operations.context,
                                    PairwiseV2ContextualAdmissionStage::owner_preparation);
        state = std::make_unique<PairwiseV2ContextualAdmission::State>(
            std::move(context), std::move(transcript), std::move(proof), std::move(runner));
    } catch (const std::bad_alloc&) {
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(proof), PairwiseV2ContextualAdmissionStage::owner_preparation,
            PairwiseV2ContextualAdmissionBranch::allocation}};
    } catch (const std::exception&) {
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(proof),
            PairwiseV2ContextualAdmissionStage::owner_preparation,
            PairwiseV2ContextualAdmissionBranch::production_contained_exception}};
    } catch (...) {
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(proof),
            PairwiseV2ContextualAdmissionStage::owner_preparation,
            PairwiseV2ContextualAdmissionBranch::production_contained_unknown_failure}};
    }

    const auto stage_fault = [&](PairwiseV2ContextualAdmissionStage stage) noexcept
        -> std::optional<PairwiseV2ContextualAdmissionBranch> {
        try {
            if (operations.before_stage)
                operations.before_stage(operations.context, stage);
            return std::nullopt;
        } catch (const std::bad_alloc&) {
            return PairwiseV2ContextualAdmissionBranch::allocation;
        } catch (const std::exception&) {
            return PairwiseV2ContextualAdmissionBranch::production_contained_exception;
        } catch (...) {
            return PairwiseV2ContextualAdmissionBranch::production_contained_unknown_failure;
        }
    };
    if (auto fault = stage_fault(PairwiseV2ContextualAdmissionStage::first_placement))
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(*state->proof), PairwiseV2ContextualAdmissionStage::first_placement,
            *fault}};
    auto first = PairwiseV2AdmissionClassificationAccess::admit_placement(
                                 state->transcript.first_request_id(),
                                 state->transcript.first(), state->context);
    if (!first)
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(*state->proof), PairwiseV2ContextualAdmissionStage::first_placement,
            first.error().branch_}};
    if (auto fault = stage_fault(PairwiseV2ContextualAdmissionStage::second_placement))
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(*state->proof), PairwiseV2ContextualAdmissionStage::second_placement,
            *fault}};
    auto second = PairwiseV2AdmissionClassificationAccess::admit_placement(
                                  state->transcript.second_request_id(),
                                  state->transcript.second(), state->context);
    if (!second)
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(*state->proof), PairwiseV2ContextualAdmissionStage::second_placement,
            second.error().branch_}};
    if (auto fault = stage_fault(PairwiseV2ContextualAdmissionStage::pair_projection))
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(*state->proof), PairwiseV2ContextualAdmissionStage::pair_projection,
            *fault}};
    auto inputs = PairwiseV2AdmissionClassificationAccess::create_inputs(
        std::move(*first), std::move(*second),
        state->transcript.resolved_comparison_order_seed(),
        state->transcript.statistics_source());
    if (!inputs)
        return std::unexpected{PairwiseV2ContextualAdmissionFailure{
            std::move(*state->proof), PairwiseV2ContextualAdmissionStage::pair_projection,
            inputs.error().branch_}};
    state->inputs.emplace(std::move(*inputs));
    return PairwiseV2ContextualAdmission{std::move(state)};
}

auto PairwiseV2ContextualAdmissionAccess::reject(
    PairwiseV2ContextualAdmissionFailure failure) noexcept
    -> HostCapacityRejectedAckCapability {
    if (!std::exchange(failure.active_, false) || !failure.proof_.active_)
        std::terminate();
    auto& proof = failure.proof_;
    proof.active_ = false;
    return HostCapacityRejectedAckCapability{
        proof.first_, proof.second_, proof.request_, proof.offer_,
        pairwise_v2_contextual_rejection_category(failure.branch_)};
}

auto PairwiseV2ContextualAdmissionAccess::gate_b(
    PairwiseV2ContextualAdmission admission) noexcept
    -> std::expected<AdmittedPairwiseHostCapacityStartup,
                     HostCapacityRejectedAckCapability> {
    auto result = gate_b_with_inputs(std::move(admission));
    if (!result)
        return std::unexpected{std::move(result.error())};
    return std::move(result->startup_);
}

auto PairwiseV2ContextualAdmissionAccess::gate_b_with_inputs(
    PairwiseV2ContextualAdmission admission) noexcept
    -> std::expected<PairwiseV2AdmittedStartup, HostCapacityRejectedAckCapability> {
    if (!admission.state_ || !admission.state_->inputs || !admission.state_->proof ||
        !admission.state_->runner)
        std::terminate();
    auto startup = PairwiseMonitoringHostCapacityStartupAccess::gate_b(
        *admission.state_->inputs, std::move(*admission.state_->proof),
        std::move(*admission.state_->runner));
    if (!startup)
        return std::unexpected{std::move(startup.error())};
    return PairwiseV2AdmittedStartup{std::move(admission), std::move(*startup)};
}

auto PairwiseV2AdmittedStartup::matches_request(
    ipc::RequestId first, ipc::RequestId second, const core::Sha256Digest& digest) const noexcept
    -> bool {
    return owner_.state_ && owner_.state_->inputs &&
           owner_.state_->transcript.first_request_id() == first &&
           owner_.state_->transcript.second_request_id() == second &&
           owner_.state_->transcript.body_sha256() == digest;
}

auto PairwiseV2AdmittedStartup::request() const noexcept
    -> const PairwiseHostCapacityRequestTranscript& {
    return owner_.state_->transcript;
}

auto PairwiseV2AdmittedStartup::inputs() const noexcept
    -> const AdmittedPairwiseProductionInputs& {
    return *owner_.state_->inputs;
}

} // namespace epycbench::runtime::detail
