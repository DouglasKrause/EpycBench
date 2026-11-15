#include "runtime/pairwise_production_run_core.hpp"

#include <exception>

namespace epycbench::runtime::detail {
namespace {

constexpr auto destination_index(PairwiseTypedFailureDestination value) noexcept
    -> std::size_t {
    return static_cast<std::size_t>(value);
}

auto preparation_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return PairwiseEvidenceFailure{*value};
}

auto preparation_operation_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseOperationFailure {
    return {preparation_failure(reason), {}, std::nullopt};
}

class BoundPairwiseProductionRun final
    : public PairwiseProductionRun::Implementation {
  public:
    BoundPairwiseProductionRun() noexcept = default;
    void bind(PairwiseProductionRunCore&& core) noexcept { core_.emplace(std::move(core)); }
    explicit BoundPairwiseProductionRun(PairwiseProductionRunCore&& core) noexcept
        : core_{std::move(core)} {}
    auto calibrate_bandwidth(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome> override {
        return core_->calibrate_bandwidth(policy);
    }
    auto calibrate_latency(const measurement::CalibrationDurationPolicy& policy) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyCalibrationOutcome> override {
        return core_->calibrate_latency(policy);
    }
    auto warmup_bandwidth(const measurement::WarmupPolicy& policy,
                          kernel::SequentialReadBandwidthPassCount work) noexcept
        -> PairwiseOperationResult<measurement::SequentialReadBandwidthWarmupOutcome> override {
        return core_->warmup_bandwidth(policy, work);
    }
    auto warmup_latency(const measurement::WarmupPolicy& policy,
                        kernel::DependentLoadLatencyStepCount work) noexcept
        -> PairwiseOperationResult<measurement::DependentLoadLatencyWarmupOutcome> override {
        return core_->warmup_latency(policy, work);
    }
    auto observe_bandwidth(
        const measurement::SequentialReadBandwidthObservationRequiredAction& action) noexcept
        -> PairwiseOperationResult<FactualObservationEvidence> override {
        return core_->observe_bandwidth(action);
    }
    auto observe_latency(
        const measurement::DependentLoadLatencyObservationRequiredAction& action) noexcept
        -> PairwiseOperationResult<FactualObservationEvidence> override {
        return core_->observe_latency(action);
    }
    auto finalize_pairwise() noexcept -> PairwiseFinalizationOutcome override {
        return std::move(*core_).finalize();
    }
  private:
    std::optional<PairwiseProductionRunCore> core_;
};

} // namespace

auto materialize_pairwise_owner_preparation_failure(
    PairwiseTypedReleasedOwnerPreparationFailure&& released) noexcept
    -> PairwiseOperationFailure {
    released.require_active();
    auto& storage = released.storage_;
    auto& retained = storage.storage_[destination_index(
        PairwiseTypedFailureDestination::preparation_secondary)];
    std::vector<PairwiseEvidenceFailure> secondary;
    if (!storage.cleanup_overflow_) {
        secondary = std::move(retained);
    } else {
        auto& overflow = storage.storage_[destination_index(
            PairwiseTypedFailureDestination::preparation_overflow)];
        if (!overflow.empty() || !storage.first_cleanup_overflow_ ||
            overflow.capacity() != retained.capacity() + 2U ||
            retained.size() != retained.capacity())
            std::terminate();
        for (auto& value : retained)
            overflow.push_back(std::move(value));
        overflow.push_back(std::move(*storage.first_cleanup_overflow_));
        overflow.push_back(std::move(storage.cleanup_overflow_invariant_));
        secondary = std::move(overflow);
    }
    released.active_ = false;
    return {std::move(released.primary_), std::move(secondary), std::nullopt};
}

PairwiseProductionRunCore::PairwiseProductionRunCore(
    PairwiseTypedPreparedOwners&& owners,
    PairwiseProductionRunFunctions functions) noexcept
    : owners_{std::move(owners)}, functions_{functions} {}

PairwiseProductionRunCore::PairwiseProductionRunCore(
    PairwiseProductionRunCore&& other) noexcept
    : owners_{std::move(other.owners_)}, functions_{other.functions_},
      state_{std::exchange(other.state_, PairwiseRunState::moved_from)} {}

void PairwiseProductionRunCore::require_progression() const noexcept {
    if (state_ != PairwiseRunState::active)
        std::terminate();
}

template <class T, class Invoke>
auto PairwiseProductionRunCore::invoke_operation(
    PairwiseCoreOperation operation, Invoke&& invoke) noexcept
    -> PairwiseOperationResult<T> {
    require_progression();
    auto result = invoke();
    if (result)
        return std::move(*result);
    return std::unexpected{
        materialize_failure(operation, std::move(result.error()))};
}

auto PairwiseProductionRunCore::take_failure_values(
    PairwiseTypedAdapterStorage& storage,
    PairwiseTypedFailureDestination destination) noexcept
    -> std::vector<PairwiseEvidenceFailure> {
    return std::move(storage.storage_[destination_index(destination)]);
}

auto PairwiseProductionRunCore::materialize_failure(
    PairwiseCoreOperation operation,
    PairwiseTypedOperationFailureSource source) noexcept
    -> PairwiseOperationFailure {
    if (operation == PairwiseCoreOperation::finalize ||
        operation == PairwiseCoreOperation::count ||
        state_ != PairwiseRunState::active)
        std::terminate();
    auto& storage = owners_.storage_;
    auto& destination = storage.storage_[destination_index(
        PairwiseTypedFailureDestination::retained_lower)];
    if (source.secondary.size() > destination.capacity() - destination.size())
        std::terminate();
    for (const auto& secondary : source.secondary)
        destination.push_back(secondary);
    state_ = PairwiseRunState::failed;
    return {std::move(source.primary),
            take_failure_values(
                storage, PairwiseTypedFailureDestination::retained_lower),
            source.active_block_id};
}

auto PairwiseProductionRunCore::calibrate_bandwidth(
    const measurement::CalibrationDurationPolicy& policy) noexcept
    -> PairwiseOperationResult<
        measurement::SequentialReadBandwidthCalibrationOutcome> {
    return invoke_operation<measurement::SequentialReadBandwidthCalibrationOutcome>(
        PairwiseCoreOperation::calibrate_bandwidth,
        [&] { return functions_.calibrate_bandwidth(owners_.context(), policy); });
}

auto PairwiseProductionRunCore::calibrate_latency(
    const measurement::CalibrationDurationPolicy& policy) noexcept
    -> PairwiseOperationResult<
        measurement::DependentLoadLatencyCalibrationOutcome> {
    return invoke_operation<measurement::DependentLoadLatencyCalibrationOutcome>(
        PairwiseCoreOperation::calibrate_latency,
        [&] { return functions_.calibrate_latency(owners_.context(), policy); });
}

auto PairwiseProductionRunCore::warmup_bandwidth(
    const measurement::WarmupPolicy& policy,
    kernel::SequentialReadBandwidthPassCount work) noexcept
    -> PairwiseOperationResult<
        measurement::SequentialReadBandwidthWarmupOutcome> {
    return invoke_operation<measurement::SequentialReadBandwidthWarmupOutcome>(
        PairwiseCoreOperation::warmup_bandwidth,
        [&] { return functions_.warmup_bandwidth(owners_.context(), policy, work); });
}

auto PairwiseProductionRunCore::warmup_latency(
    const measurement::WarmupPolicy& policy,
    kernel::DependentLoadLatencyStepCount work) noexcept
    -> PairwiseOperationResult<
        measurement::DependentLoadLatencyWarmupOutcome> {
    return invoke_operation<measurement::DependentLoadLatencyWarmupOutcome>(
        PairwiseCoreOperation::warmup_latency,
        [&] { return functions_.warmup_latency(owners_.context(), policy, work); });
}

auto PairwiseProductionRunCore::observe_bandwidth(
    const measurement::SequentialReadBandwidthObservationRequiredAction& action)
    noexcept -> PairwiseOperationResult<FactualObservationEvidence> {
    return invoke_operation<FactualObservationEvidence>(
        PairwiseCoreOperation::observe_bandwidth,
        [&] { return functions_.observe_bandwidth(owners_.context(), action); });
}

auto PairwiseProductionRunCore::observe_latency(
    const measurement::DependentLoadLatencyObservationRequiredAction& action)
    noexcept -> PairwiseOperationResult<FactualObservationEvidence> {
    return invoke_operation<FactualObservationEvidence>(
        PairwiseCoreOperation::observe_latency,
        [&] { return functions_.observe_latency(owners_.context(), action); });
}

auto PairwiseProductionRunCore::finalize() && noexcept
    -> PairwiseFinalizationOutcome {
    if (state_ != PairwiseRunState::active &&
        state_ != PairwiseRunState::failed)
        std::terminate();
    state_ = PairwiseRunState::finalized;
    auto source = functions_.finalize(owners_.context());
    auto& prepared_storage = owners_.storage_;
    auto& associations = prepared_storage.destinations_.finalization_associations;
    auto& errors = prepared_storage.storage_[destination_index(
        PairwiseTypedFailureDestination::finalization_local)];
    if (source.associations.size() > associations.capacity() - associations.size() ||
        source.errors.size() > errors.capacity() - errors.size())
        std::terminate();
    for (const auto& association : source.associations)
        associations.push_back(association);
    for (auto& error : source.errors)
        errors.push_back(std::move(error));
    auto storage = std::move(owners_).retire();
    return {std::move(source.evidence),
            std::move(storage.destinations_.finalization_associations),
            take_failure_values(
                storage, PairwiseTypedFailureDestination::finalization_local)};
}

auto prepare_pairwise_production_run(
    PairwiseTypedPreparedOwners&& owners,
    PairwiseProductionRunFunctions functions) noexcept
    -> PairwiseOperationResult<PairwiseProductionRunCore> {
    auto bound_owners = std::move(owners);
    if (bound_owners.context() == nullptr ||
        functions.calibrate_bandwidth == nullptr ||
        functions.calibrate_latency == nullptr ||
        functions.warmup_bandwidth == nullptr ||
        functions.warmup_latency == nullptr ||
        functions.observe_bandwidth == nullptr ||
        functions.observe_latency == nullptr ||
        functions.finalize == nullptr) {
        static_cast<void>(std::move(bound_owners).retire());
        return std::unexpected{preparation_operation_failure(
            PairwiseFailureReason::contained_exception)};
    }
    return PairwiseProductionRunCore{
        std::move(bound_owners), functions};
}

auto PreparedPairwiseProductionRun::auxiliary_bytes() noexcept -> std::size_t {
    return sizeof(BoundPairwiseProductionRun);
}

auto PreparedPairwiseProductionRun::prepare() noexcept
    -> PairwiseOperationResult<PreparedPairwiseProductionRun> {
    try {
        return PreparedPairwiseProductionRun{std::make_unique<BoundPairwiseProductionRun>()};
    } catch (...) {
        return std::unexpected{preparation_operation_failure(
            PairwiseFailureReason::contained_exception)};
    }
}

auto PreparedPairwiseProductionRun::bind(PairwiseProductionRunCore&& core) && noexcept
    -> PairwiseOperationResult<PairwiseProductionRun> {
    if (!destination_)
        return std::unexpected{preparation_operation_failure(
            PairwiseFailureReason::contained_exception)};
    static_cast<BoundPairwiseProductionRun*>(destination_.get())->bind(std::move(core));
    return PairwiseProductionRun{std::move(destination_)};
}

auto make_pairwise_production_run(PairwiseProductionRunCore&& core) noexcept
    -> PairwiseOperationResult<PairwiseProductionRun> {
    try {
        return PairwiseProductionRun{
            std::make_unique<BoundPairwiseProductionRun>(std::move(core))};
    } catch (...) {
        return std::unexpected{preparation_operation_failure(
            PairwiseFailureReason::contained_exception)};
    }
}

auto prepare_pairwise_production_run(
    const PairwiseProductionOperations& operations,
    const AdmittedPairwiseProductionInputs& inputs) noexcept
    -> PairwiseOperationResult<PairwiseProductionRun> {
    if (!operations.context || operations.prepare_run == nullptr)
        return std::unexpected{preparation_operation_failure(
            PairwiseFailureReason::contained_exception)};
    return operations.prepare_run(operations.context.get(), inputs);
}

} // namespace epycbench::runtime::detail
