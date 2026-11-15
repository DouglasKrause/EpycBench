#include "runtime/pairwise_typed_local_monitoring_capacity_producer.hpp"

#include <utility>

namespace epycbench::runtime::detail {
namespace {
using LowerFailure = platform_linux::detail::LinuxMonitoringCapacityFailure;
using LowerKind = platform_linux::detail::LinuxMonitoringCapacityFailureKind;
using AdmissionFailure = PairwiseMonitoringHostCapacityAdmissionFailure;

auto platform_failure(PairwiseFailureCode code, PairwiseFailureReason reason,
                      PairwiseMonitoringCapacityAcquisitionLeg leg, bool uncertain,
                      int native) noexcept -> PairwiseEvidenceFailure {
    auto value = PairwisePlatformFailure::create(
        code, PairwiseFailureOperation::linux_monitoring_capacity_authenticate, reason,
        PairwiseMonitoringCapacityAcquisitionContext{leg, uncertain},
        native == 0 ? std::nullopt : std::optional<int>{native});
    if (!value) std::terminate();
    return PairwiseEvidenceFailure{std::move(*value)};
}

auto reason(LowerKind kind) noexcept -> PairwiseFailureReason {
    switch (kind) {
    case LowerKind::invalid_host_facts: return PairwiseFailureReason::invalid_host_facts;
    case LowerKind::allocation: return PairwiseFailureReason::allocation;
    case LowerKind::open: return PairwiseFailureReason::open;
    case LowerKind::held_stat: return PairwiseFailureReason::held_stat;
    case LowerKind::named_stat: return PairwiseFailureReason::named_stat;
    case LowerKind::identity: return PairwiseFailureReason::identity;
    case LowerKind::read: return PairwiseFailureReason::read;
    case LowerKind::byte_limit: return PairwiseFailureReason::byte_limit;
    case LowerKind::invalid_value: return PairwiseFailureReason::invalid_value;
    case LowerKind::close: return PairwiseFailureReason::close;
    case LowerKind::arithmetic_overflow:
    case LowerKind::read_contract:
    case LowerKind::contained_exception:
    case LowerKind::contained_unknown_failure:
    case LowerKind::inactive: break;
    }
    std::terminate();
}

auto project_lower(const LowerFailure& failure,
                   PairwiseMonitoringCapacityAcquisitionLeg leg) noexcept
    -> PairwiseTypedLocalMonitoringCapacityFailure {
    auto result = PairwiseTypedLocalMonitoringCapacityFailure{
        platform_failure(PairwiseFailureCode::platform_monitoring_capacity_acquisition_failed,
                         reason(failure.primary), leg, failure.primary_uncertain,
                         failure.native_error), std::nullopt};
    if (failure.cleanup)
        result.cleanup = platform_failure(
            PairwiseFailureCode::platform_monitoring_capacity_cleanup_failed,
            PairwiseFailureReason::close, leg, failure.cleanup->uncertain,
            failure.cleanup->native_error);
    return result;
}

auto runtime_failure(AdmissionFailure failure) noexcept -> PairwiseEvidenceFailure {
    PairwiseFailureReason mapped{};
    switch (failure) {
    case AdmissionFailure::allocation: mapped = PairwiseFailureReason::allocation; break;
    case AdmissionFailure::lower_authority_mismatch:
        mapped = PairwiseFailureReason::lower_authority_mismatch; break;
    default: std::terminate();
    }
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_monitoring_capacity_admission_failed,
        PairwiseFailureOperation::runtime_monitoring_capacity_admit, mapped,
        NoPairwiseFailureContext{});
    if (!value) std::terminate();
    return PairwiseEvidenceFailure{std::move(*value)};
}
} // namespace

PairwiseTypedLocalMonitoringCapacityAcceptance::
PairwiseTypedLocalMonitoringCapacityAcceptance(
    PairwiseMonitoringHostCapacityAdmission admission, ipc::RequestId first,
    ipc::RequestId second, AdmittedPlacementIdentity first_identity,
    AdmittedPlacementIdentity second_identity, void* retirement_context,
    void (*retirement_observer)(void*) noexcept) noexcept
    : admission_{std::move(admission)}, first_request_{first}, second_request_{second},
      first_identity_{std::move(first_identity)}, second_identity_{std::move(second_identity)},
      retirement_context_{retirement_context}, retirement_observer_{retirement_observer},
      active_{true} {}

PairwiseTypedLocalMonitoringCapacityAcceptance::
PairwiseTypedLocalMonitoringCapacityAcceptance(
    PairwiseTypedLocalMonitoringCapacityAcceptance&& other) noexcept
    : admission_{std::move(other.admission_)}, first_request_{other.first_request_},
      second_request_{other.second_request_},
      first_identity_{std::move(other.first_identity_)},
      second_identity_{std::move(other.second_identity_)},
      retirement_context_{other.retirement_context_},
      retirement_observer_{other.retirement_observer_},
      active_{std::exchange(other.active_, false)} {}

PairwiseTypedLocalMonitoringCapacityAcceptance::~PairwiseTypedLocalMonitoringCapacityAcceptance()
    noexcept { retire(); }
void PairwiseTypedLocalMonitoringCapacityAcceptance::retire() noexcept {
    if (!std::exchange(active_, false)) return;
    admission_.reset();
    if (retirement_observer_) retirement_observer_(retirement_context_);
}

auto PairwiseTypedLocalMonitoringCapacityProducer::prepare(
    const AdmittedPairwiseProductionInputs& inputs) const noexcept
    -> std::expected<PairwiseTypedLocalMonitoringCapacityAcceptance,
                     PairwiseTypedLocalMonitoringCapacityFailure> {
    const auto acquire = [&](PairwiseVariant variant,
                             const auto& operations) {
        return operations.open == nullptr
            ? PairwiseMonitoringHostCapacityAdmissionAccess::mint(inputs, variant)
            : PairwiseMonitoringHostCapacityAdmissionAccess::mint_with(inputs, variant,
                                                                        operations);
    };
    auto first = acquire(PairwiseVariant::first, dependencies_.first);
    if (!first) return std::unexpected{project_lower(
        first.error(), PairwiseMonitoringCapacityAcquisitionLeg::first_controller)};
    auto second = acquire(PairwiseVariant::second, dependencies_.second);
    if (!second) return std::unexpected{project_lower(
        second.error(), PairwiseMonitoringCapacityAcquisitionLeg::second_runner)};
    auto admission = [&]() noexcept
        -> std::expected<PairwiseMonitoringHostCapacityAdmission, AdmissionFailure> {
        try {
            if (dependencies_.deny_admission_allocation)
                throw std::bad_alloc{};
            return PairwiseMonitoringHostCapacityAdmissionAccess::project(
                inputs, std::move(*first), std::move(*second),
                PairwiseMonitoringHostCapacityAdmissionAccess::expected_capacity_identity(),
                PairwiseMonitoringHostCapacityAdmissionAccess::expected_profile_identity());
        } catch (const std::bad_alloc&) {
            return std::unexpected{AdmissionFailure::allocation};
        } catch (...) {
            std::terminate();
        }
    }();
    if (!admission)
        return std::unexpected{PairwiseTypedLocalMonitoringCapacityFailure{
            runtime_failure(admission.error()), std::nullopt}};
    const auto& pair = inputs.pairwise_request();
    return PairwiseTypedLocalMonitoringCapacityAcceptance{
        std::move(*admission), pair.first().request_id(), pair.second().request_id(),
        pair.first().placement_identity(), pair.second().placement_identity(),
        dependencies_.retirement_context, dependencies_.retirement_observer};
}

auto PairwiseTypedLocalMonitoringCapacityProducer::release(
    PairwiseTypedLocalMonitoringCapacityAcceptance&& accepted) noexcept
    -> std::expected<PairwiseTypedMonitoringCapacityHandoff,
                     PairwiseTypedMonitoringCapacityBindingFailure> {
    if (!accepted.active_ || !accepted.admission_)
        return std::unexpected{PairwiseTypedMonitoringCapacityBindingFailure{
            PairwiseTypedMonitoringCapacityBindingFailureKind::already_consumed}};
    accepted.active_ = false;
    return PairwiseTypedMonitoringCapacityHandoff{
        std::move(*accepted.admission_), accepted.first_request_, accepted.second_request_,
        std::move(accepted.first_identity_), std::move(accepted.second_identity_),
        accepted.retirement_context_, accepted.retirement_observer_};
}

} // namespace epycbench::runtime::detail
