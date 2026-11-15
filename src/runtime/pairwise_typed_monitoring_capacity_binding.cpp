#include "runtime/pairwise_typed_monitoring_capacity_binding.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto service_cpu(const AdmittedPairwiseProductionInputs& inputs) noexcept
    -> std::optional<core::LinuxCpuId> {
    std::optional<core::LinuxCpuId> result;
    for (const auto& assignment : inputs.service_placement().assignments()) {
        if (assignment.role != ServiceRole::measurement_monitor)
            continue;
        if (result)
            return std::nullopt;
        result = assignment.linux_cpu;
    }
    return result;
}

[[nodiscard]] auto matches(
    const PairwiseMonitoringHostCapacityAdmission& admission,
    const AdmittedPairwiseProductionInputs& inputs) noexcept -> bool {
    const auto& pair = inputs.pairwise_request();
    const auto& first = pair.first();
    const auto& second = pair.second();
    const auto& measurement = first.execution_inputs().measurement();
    if (first.request_id() == second.request_id() ||
        first.placement_identity() == second.placement_identity() ||
        !PairwiseMonitoringHostCapacityAdmissionAccess::
             provenance_matches(admission, inputs) ||
        !(first.context().fingerprints() == second.context().fingerprints() &&
                  admission.authority().physical_fingerprint() ==
                      first.context().fingerprints().physical &&
                  admission.authority().effective_fingerprint() ==
                      first.context().fingerprints().effective) ||
        first.execution_inputs().measurement() !=
            second.execution_inputs().measurement() ||
        first.workers().size() != second.workers().size() ||
        admission.worker_count() != first.workers().size() ||
        admission.maximum_blocks() != measurement.repetition_maximum_blocks() ||
        admission.maximum_observation_nanoseconds() !=
            measurement.observation_maximum_duration().value() ||
        admission.monitoring_policy() != inputs.monitoring_policy() ||
        admission.capacity_contract_identity() !=
            PairwiseMonitoringHostCapacityAdmissionAccess::expected_capacity_identity() ||
        admission.finalized_error_profile_identity() !=
            PairwiseMonitoringHostCapacityAdmissionAccess::expected_profile_identity() ||
        !admission.authority().authentically_bound())
        return false;
    const auto expected_interval = inputs.monitoring_policy().during_interval()
        ? std::optional{inputs.monitoring_policy().during_interval()->value()}
        : std::nullopt;
    if (admission.interval_nanoseconds().has_value() !=
            expected_interval.has_value() ||
        admission.interval_nanoseconds() != expected_interval ||
        admission.service_cpu() != service_cpu(inputs))
        return false;
    bool ordered_cpus = admission.worker_cpus().size() == first.workers().size();
    for (std::size_t index{}; index < first.workers().size(); ++index) {
        const auto cpu = first.workers()[index].request().cpu().linux_cpu();
        if (cpu != second.workers()[index].request().cpu().linux_cpu() ||
            !ordered_cpus || admission.worker_cpus()[index] != cpu)
            ordered_cpus = false;
    }
    return ordered_cpus;
}

} // namespace

PairwiseTypedMonitoringCapacityBinding::PairwiseTypedMonitoringCapacityBinding(
    PairwiseMonitoringHostCapacityAdmission admission,
    ipc::RequestId first, ipc::RequestId second,
    AdmittedPlacementIdentity first_identity,
    AdmittedPlacementIdentity second_identity,
    void* retirement_context,
    void (*retirement_observer)(void*) noexcept) noexcept
    : admission_{std::move(admission)}, first_request_{first},
      second_request_{second}, first_identity_{std::move(first_identity)},
      second_identity_{std::move(second_identity)},
      retirement_context_{retirement_context},
      retirement_observer_{retirement_observer} {}

PairwiseTypedMonitoringCapacityBinding::PairwiseTypedMonitoringCapacityBinding(
    PairwiseTypedMonitoringCapacityBinding&& other) noexcept
    : admission_{std::move(other.admission_)}, first_request_{other.first_request_},
      second_request_{other.second_request_},
      first_identity_{std::move(other.first_identity_)},
      second_identity_{std::move(other.second_identity_)},
      consumed_{std::exchange(other.consumed_, true)},
      retirement_active_{std::exchange(other.retirement_active_, false)},
      retirement_context_{other.retirement_context_},
      retirement_observer_{other.retirement_observer_} {
    other.admission_.reset();
}

PairwiseTypedMonitoringCapacityBinding::~PairwiseTypedMonitoringCapacityBinding() noexcept {
    retire();
}

void PairwiseTypedMonitoringCapacityBinding::retire() noexcept {
    if (!std::exchange(retirement_active_, false))
        return;
    admission_.reset();
    if (retirement_observer_)
        retirement_observer_(retirement_context_);
}

auto PairwiseTypedMonitoringCapacityBinding::create(
    PairwiseTypedMonitoringCapacityHandoff handoff,
    const AdmittedPairwiseProductionInputs& inputs) noexcept
    -> std::expected<PairwiseTypedMonitoringCapacityBinding,
                     PairwiseTypedMonitoringCapacityBindingFailure> {
    const auto& pair = inputs.pairwise_request();
    if (!handoff.active_ || !handoff.admission_ ||
        handoff.first_request_ != pair.first().request_id() ||
        handoff.second_request_ != pair.second().request_id() ||
        handoff.first_identity_ != pair.first().placement_identity() ||
        handoff.second_identity_ != pair.second().placement_identity() ||
        !matches(*handoff.admission_, inputs))
        return std::unexpected{
            PairwiseTypedMonitoringCapacityBindingFailure{
                PairwiseTypedMonitoringCapacityBindingFailureKind::foreign_inputs}};
    auto result = PairwiseTypedMonitoringCapacityBinding{
        std::move(*handoff.admission_), pair.first().request_id(), pair.second().request_id(),
        pair.first().placement_identity(), pair.second().placement_identity(),
        handoff.retirement_context_, handoff.retirement_observer_};
    handoff.active_ = false;
    handoff.retirement_observer_ = nullptr;
    handoff.admission_.reset();
    return result;
}

auto PairwiseTypedMonitoringCapacityBinding::consume(
    const AdmittedPairwiseProductionInputs& inputs) && noexcept
    -> std::expected<PairwiseAdmittedMonitoringCapacity,
                     PairwiseTypedMonitoringCapacityBindingFailure> {
    if (consumed_ || !admission_)
        return std::unexpected{
            PairwiseTypedMonitoringCapacityBindingFailure{
                PairwiseTypedMonitoringCapacityBindingFailureKind::already_consumed}};
    consumed_ = true;
    const auto& pair = inputs.pairwise_request();
    if (pair.first().request_id() != first_request_ ||
        pair.second().request_id() != second_request_ ||
        pair.first().placement_identity() != first_identity_ ||
        pair.second().placement_identity() != second_identity_ ||
        !matches(*admission_, inputs)) {
        retire();
        return std::unexpected{
            PairwiseTypedMonitoringCapacityBindingFailure{
                PairwiseTypedMonitoringCapacityBindingFailureKind::foreign_inputs}};
    }
    auto projected = PairwiseMonitoringCapacityAccess::project(
        std::move(*admission_));
    admission_.reset();
    if (!projected) {
        auto failure = PairwiseTypedMonitoringCapacityBindingFailure{
            std::move(projected.error())};
        retire();
        return std::unexpected{std::move(failure)};
    }
    if (std::exchange(retirement_active_, false) && retirement_observer_)
        retirement_observer_(retirement_context_);
    return std::move(*projected);
}

auto PairwiseTypedMonitoringPrepareContext::prepare(
    const AdmittedPairwiseProductionInputs& inputs) noexcept
    -> std::expected<PairwiseAdmittedMonitoringCapacity,
                     PairwiseTypedMonitoringCapacityBindingFailure> {
    if (attempted_ || !binding_)
        return std::unexpected{
            PairwiseTypedMonitoringCapacityBindingFailure{
                PairwiseTypedMonitoringCapacityBindingFailureKind::already_consumed}};
    attempted_ = true;
    auto binding = std::move(*binding_);
    binding_.reset();
    return std::move(binding).consume(inputs);
}

} // namespace epycbench::runtime::detail
