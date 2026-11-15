#include "runtime/pairwise_typed_adapter_storage.hpp"

#include <limits>
#include <new>

namespace epycbench::runtime::detail {
namespace {
thread_local void* allocation_context{};
thread_local void (*allocation_checkpoint)(void*, std::size_t, bool){};
auto checked(std::size_t value, std::size_t multiplier, std::size_t addition)
    -> std::optional<std::size_t> {
    if (value > (std::numeric_limits<std::size_t>::max() - addition) / multiplier)
        return std::nullopt;
    return value * multiplier + addition;
}
auto checked_observation_capacities(std::size_t workers) noexcept
    -> std::optional<std::array<std::size_t, 2U>> {
    const auto maximum = std::max(workers, std::size_t{2U});
    const auto secondary_base = checked(workers, 1U, 1U);
    const auto global_base = checked(workers, 7U, 8U);
    if (!secondary_base || !global_base ||
        *secondary_base > std::numeric_limits<std::size_t>::max() - maximum ||
        *global_base > std::numeric_limits<std::size_t>::max() - maximum)
        return std::nullopt;
    return std::array<std::size_t, 2U>{
        *secondary_base + maximum, *global_base + maximum};
}
constexpr auto index(PairwiseTypedFailureDestination value) noexcept -> std::size_t {
    return static_cast<std::size_t>(value);
}
auto preparation_failure(PairwiseFailureReason reason) noexcept -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value) std::terminate();
    return PairwiseEvidenceFailure{*value};
}
auto make_cleanup_overflow_invariant(std::size_t capacity) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::force_error_capacity_exhausted,
        ExpectedObservedCountContext{PairwiseCount{capacity},
                                     PairwiseCount{capacity + 1U}});
    if (!value) std::terminate();
    return PairwiseEvidenceFailure{*value};
}
} // namespace

struct PairwiseMonitoringOperationFailureAccess final {
    static auto lift(const PairwiseMonitoringOperationFailure& source) noexcept
        -> PairwiseEvidenceFailure {
        return std::visit([](const auto& value) -> PairwiseEvidenceFailure {
            return value;
        }, source.value_);
    }
};

PairwiseTypedAdapterStorage::PairwiseTypedAdapterStorage(
    std::size_t workers, std::size_t blocks,
    std::array<std::vector<PairwiseEvidenceFailure>,
               static_cast<std::size_t>(PairwiseTypedFailureDestination::count)>&& storage,
    DestinationWorkspace&& destinations) noexcept
    : workers_{workers}, blocks_{blocks}, factual_destinations_{blocks * 4U},
      finalization_associations_{workers * 2U}, storage_{std::move(storage)},
      destinations_{std::move(destinations)},
      cleanup_overflow_invariant_{make_cleanup_overflow_invariant(
          storage_[index(PairwiseTypedFailureDestination::projected_cleanup)].capacity())} {}

auto PairwiseTypedAdapterStorage::create(std::size_t workers,
                                         std::size_t maximum_blocks) noexcept
    -> std::expected<PairwiseTypedAdapterStorage, PairwiseEvidenceFailure> {
    const auto preparation = checked(workers, 6U, 6U);
    const auto retained = checked(workers, 4U, 3U);
    const auto forced = checked(workers, 2U, 3U);
    const auto preparation_overflow = checked(workers, 6U, 8U);
    const auto observation_capacities = checked_observation_capacities(workers);
    const auto destination_count = checked(maximum_blocks, 4U, 0U);
    const auto associations = checked(workers, 2U, 0U);
    if (!preparation || !preparation_overflow || !retained || !forced ||
        !observation_capacities || !destination_count || !associations)
        return std::unexpected{preparation_failure(
            PairwiseFailureReason::contained_exception)};
    try {
        std::array<std::vector<PairwiseEvidenceFailure>,
                   static_cast<std::size_t>(PairwiseTypedFailureDestination::count)> result;
        const std::array capacities{*preparation, *preparation_overflow,
                                    *preparation, *retained, *forced,
                                    *preparation, (*observation_capacities)[1]};
        std::size_t allocation_ordinal{};
        for (std::size_t slot = 0; slot < result.size(); ++slot) {
            if (allocation_checkpoint)
                allocation_checkpoint(allocation_context, allocation_ordinal++, false);
            result[slot].reserve(capacities[slot]);
        }
        DestinationWorkspace destinations;
        auto reserve = [&](auto& value, std::size_t count) {
            if (allocation_checkpoint)
                allocation_checkpoint(allocation_context, allocation_ordinal++, false);
            value.reserve(count);
        };
        reserve(destinations.blocks, maximum_blocks);
        reserve(destinations.stopping, maximum_blocks);
        reserve(destinations.effects, maximum_blocks);
        reserve(destinations.observations, *destination_count);
        for (std::size_t slot = 0; slot < *destination_count; ++slot) {
            destinations.observations.emplace_back();
            reserve(destinations.observations.back().placements, workers);
            reserve(destinations.observations.back().migrations, workers);
        }
        reserve(destinations.failed_observations, maximum_blocks);
        for (std::size_t slot = 0; slot < maximum_blocks; ++slot) {
            destinations.failed_observations.emplace_back();
            reserve(destinations.failed_observations.back().secondary,
                    (*observation_capacities)[0]);
        }
        reserve(destinations.consumed_observation_ordinals, *destination_count);
        destinations.consumed_observation_ordinals.resize(*destination_count, 0U);
        reserve(destinations.finalization_associations, *associations);
        return PairwiseTypedAdapterStorage{workers, maximum_blocks, std::move(result),
                                           std::move(destinations)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{preparation_failure(
            PairwiseFailureReason::contained_exception)};
    } catch (...) {
        return std::unexpected{preparation_failure(
            PairwiseFailureReason::contained_unknown_failure)};
    }
}

auto PairwiseTypedAdapterStorage::append(PairwiseTypedFailureDestination destination,
                                         PairwiseEvidenceFailure failure) noexcept -> bool {
    auto& values = storage_[index(destination)];
    if (values.size() == values.capacity()) return false;
    values.push_back(std::move(failure));
    return true;
}
void PairwiseTypedAdapterStorage::install_observation_workers(
    PairwiseTypedPreparedObservationWorkers&& workers) noexcept {
    if (observation_workers_installed_) std::terminate();
    observation_workers_ = std::move(workers);
    observation_workers_installed_ = true;
}

auto PairwiseTypedAdapterStorage::consume_worker_callback(
    const PairwiseWorkerCallbackFailure& failure,
    PairwiseTypedFailureDestination destination) noexcept -> bool {
    const auto needed = 1U + static_cast<std::size_t>(failure.cleanup.has_value());
    if (capacity(destination) - values(destination).size() < needed) return false;
    if (!append(destination, failure.primary.failure())) std::terminate();
    return !failure.cleanup || append(destination, failure.cleanup->failure());
}

auto PairwiseTypedAdapterStorage::consume_memory_finalization(
    const PairwiseMemoryFinalizationFailure& failure,
    PairwiseTypedFailureDestination destination) noexcept -> bool {
    const auto needed = 1U + static_cast<std::size_t>(failure.cleanup.has_value());
    if (capacity(destination) - values(destination).size() < needed) return false;
    if (!append(destination, PairwiseEvidenceFailure{failure.primary})) std::terminate();
    return !failure.cleanup ||
           append(destination, PairwiseEvidenceFailure{*failure.cleanup});
}
auto PairwiseTypedAdapterStorage::consume_memory_sequence(
    const PairwiseMemoryFailureSequence& failure,
    PairwiseTypedFailureDestination destination) noexcept -> bool {
    if (capacity(destination) - values(destination).size() < failure.size()) return false;
    for (std::size_t index = 0; index < failure.size(); ++index)
        if (!append(destination, failure[index])) std::terminate();
    return true;
}

auto PairwiseTypedAdapterStorage::consume_monitoring_operation(
    const PairwiseMonitoringOperationFailure& failure,
    PairwiseTypedFailureDestination destination) noexcept -> bool {
    return append(destination, PairwiseMonitoringOperationFailureAccess::lift(failure));
}
auto PairwiseTypedAdapterStorage::append_effect(statistics::PairedLogEffect value) noexcept
    -> bool {
    if (destinations_.effects.size() == destinations_.effects.capacity()) return false;
    destinations_.effects.emplace_back(value); return true;
}
auto PairwiseTypedObservationDestination::append_placement(
    measurement::WorkerPlacementValidityEvidence value) noexcept -> bool {
    auto& values = storage_->destinations_.observations[ordinal_].placements;
    if (values.size() == values.capacity()) return false;
    values.emplace_back(std::move(value)); return true;
}
auto PairwiseTypedObservationDestination::append_migration(
    measurement::WorkerMigrationValidityEvidence value) noexcept -> bool {
    auto& values = storage_->destinations_.observations[ordinal_].migrations;
    if (values.size() == values.capacity()) return false;
    values.emplace_back(std::move(value)); return true;
}
auto PairwiseTypedObservationDestination::set_failed_primary(
    PairwiseEvidenceFailure value) noexcept -> bool {
    auto& destination = storage_->destinations_.failed_observations[ordinal_ / 4U];
    if (destination.primary) return false;
    destination.primary.emplace(std::move(value));
    return true;
}
auto PairwiseTypedObservationDestination::append_failed_secondary(
    PairwiseEvidenceFailure value) noexcept -> bool {
    auto& values = storage_->destinations_.failed_observations[ordinal_ / 4U].secondary;
    if (values.size() == values.capacity()) return false;
    values.push_back(std::move(value));
    return true;
}
auto PairwiseTypedObservationDestination::take_placements() noexcept
    -> std::vector<measurement::WorkerPlacementValidityEvidence> {
    return std::move(storage_->destinations_.observations[ordinal_].placements);
}
auto PairwiseTypedObservationDestination::take_migrations() noexcept
    -> std::vector<measurement::WorkerMigrationValidityEvidence> {
    return std::move(storage_->destinations_.observations[ordinal_].migrations);
}
auto PairwiseTypedObservationDestination::append_bandwidth_worker(
    measurement::SequentialReadBandwidthWorkerObservation value) noexcept -> bool {
    auto* destinations = std::get_if<std::vector<std::vector<
        measurement::SequentialReadBandwidthWorkerObservation>>>(
        &storage_->observation_workers_);
    if (!storage_->observation_workers_installed_ || destinations == nullptr)
        std::terminate();
    auto& values = (*destinations)[ordinal_];
    if (values.size() == values.capacity()) return false;
    values.emplace_back(std::move(value));
    return true;
}
auto PairwiseTypedObservationDestination::append_latency_worker(
    measurement::DependentLoadLatencyWorkerObservation value) noexcept -> bool {
    auto* destinations = std::get_if<std::vector<std::vector<
        measurement::DependentLoadLatencyWorkerObservation>>>(
        &storage_->observation_workers_);
    if (!storage_->observation_workers_installed_ || destinations == nullptr)
        std::terminate();
    auto& values = (*destinations)[ordinal_];
    if (values.size() == values.capacity()) return false;
    values.emplace_back(std::move(value));
    return true;
}
auto PairwiseTypedObservationDestination::take_bandwidth_workers() noexcept
    -> std::vector<measurement::SequentialReadBandwidthWorkerObservation> {
    auto* destinations = std::get_if<std::vector<std::vector<
        measurement::SequentialReadBandwidthWorkerObservation>>>(
        &storage_->observation_workers_);
    if (!storage_->observation_workers_installed_ || destinations == nullptr)
        std::terminate();
    return std::move((*destinations)[ordinal_]);
}
auto PairwiseTypedObservationDestination::take_latency_workers() noexcept
    -> std::vector<measurement::DependentLoadLatencyWorkerObservation> {
    auto* destinations = std::get_if<std::vector<std::vector<
        measurement::DependentLoadLatencyWorkerObservation>>>(
        &storage_->observation_workers_);
    if (!storage_->observation_workers_installed_ || destinations == nullptr)
        std::terminate();
    return std::move((*destinations)[ordinal_]);
}
auto PairwiseTypedAdapterStorage::effect_count() const noexcept -> std::size_t {
    return destinations_.effects.size();
}
auto PairwiseTypedAdapterStorage::placement_count(std::size_t observation) const noexcept
    -> std::size_t {
    return observation < destinations_.observations.size()
        ? destinations_.observations[observation].placements.size() : 0U;
}
auto PairwiseTypedAdapterStorage::migration_count(std::size_t observation) const noexcept
    -> std::size_t {
    return observation < destinations_.observations.size()
        ? destinations_.observations[observation].migrations.size() : 0U;
}
auto PairwiseTypedAdapterStorage::failed_observation_secondary_count(
    std::size_t block) const noexcept -> std::size_t {
    return block < destinations_.failed_observations.size()
        ? destinations_.failed_observations[block].secondary.size() : 0U;
}
auto PairwiseTypedAdapterStorage::failed_observation_secondary_capacity(
    std::size_t block) const noexcept -> std::size_t {
    return block < destinations_.failed_observations.size()
        ? destinations_.failed_observations[block].secondary.capacity() : 0U;
}
#define EPYCBENCH_TYPED_CONSUMER(name, type)                                      \
    auto PairwiseTypedAdapterStorage::consume_##name(                             \
        const type& failure, PairwiseTypedFailureDestination destination)         \
        noexcept -> bool {                                                        \
        return append(destination, PairwiseEvidenceFailure{failure});             \
    }
EPYCBENCH_TYPED_CONSUMER(platform, PairwisePlatformFailure)
EPYCBENCH_TYPED_CONSUMER(memory, PairwiseMemoryFailure)
EPYCBENCH_TYPED_CONSUMER(monitoring, PairwiseMonitoringFailure)
EPYCBENCH_TYPED_CONSUMER(kernel, PairwiseKernelFailure)
EPYCBENCH_TYPED_CONSUMER(measurement, PairwiseMeasurementFailure)
EPYCBENCH_TYPED_CONSUMER(statistics, PairwiseStatisticsFailure)
EPYCBENCH_TYPED_CONSUMER(worker, PairwiseWorkerFailure)
EPYCBENCH_TYPED_CONSUMER(runtime, PairwiseRuntimeFailure)
#undef EPYCBENCH_TYPED_CONSUMER

auto PairwiseTypedAdapterStorage::values(PairwiseTypedFailureDestination destination) const noexcept
    -> std::span<const PairwiseEvidenceFailure> { return storage_[index(destination)]; }
auto PairwiseTypedAdapterStorage::capacity(PairwiseTypedFailureDestination destination) const noexcept
    -> std::size_t { return storage_[index(destination)].capacity(); }
PairwiseTypedFailureSink::PairwiseTypedFailureSink(
    PairwiseTypedAdapterStorage& storage,
    PairwiseTypedFailureDestination destination) noexcept
    : storage_{&storage}, destination_{destination} {}
auto PairwiseTypedFailureSink::append(PairwiseEvidenceFailure failure) noexcept
    -> bool {
    const auto before = storage_->values(destination_).size();
    storage_->append_cleanup_or_mark(destination_, std::move(failure));
    return storage_->values(destination_).size() != before;
}
auto PairwiseTypedAdapterStorage::failure_sink(
    PairwiseTypedFailureDestination destination) noexcept
    -> PairwiseTypedFailureSink {
    if (destination != PairwiseTypedFailureDestination::preparation_secondary &&
        destination != PairwiseTypedFailureDestination::projected_cleanup)
        std::terminate();
    return PairwiseTypedFailureSink{*this, destination};
}
void PairwiseTypedAdapterStorage::append_cleanup_or_mark(
    PairwiseTypedFailureDestination destination,
    PairwiseEvidenceFailure&& failure) noexcept {
    if (destination != PairwiseTypedFailureDestination::preparation_secondary &&
        destination != PairwiseTypedFailureDestination::projected_cleanup)
        std::terminate();
    auto& projected = storage_[index(destination)];
    if (projected.size() < projected.capacity()) {
        projected.push_back(std::move(failure));
        return;
    }
    cleanup_overflow_ = true;
    if (!first_cleanup_overflow_)
        first_cleanup_overflow_.emplace(std::move(failure));
}
void PairwiseTypedAdapterStorageTestAccess::install_allocation_checkpoint(
    void* context, void (*checkpoint)(void*, std::size_t, bool)) noexcept {
    allocation_context = context;
    allocation_checkpoint = checkpoint;
}
void PairwiseTypedAdapterStorageTestAccess::clear_allocation_checkpoint() noexcept {
    allocation_context = nullptr;
    allocation_checkpoint = nullptr;
}
auto PairwiseTypedAdapterStorageTestAccess::observation_facts(
    const PairwiseTypedAdapterStorage& storage, std::size_t observation,
    std::size_t block) noexcept -> ObservationFacts {
    if (observation >= storage.destinations_.observations.size() ||
        block >= storage.destinations_.failed_observations.size())
        std::terminate();
    const auto& destination = storage.destinations_.observations[observation];
    const auto& failed = storage.destinations_.failed_observations[block];
    const auto& consumed = storage.destinations_.consumed_observation_ordinals;
    return {.placement = destination.placements.data(),
            .migration = destination.migrations.data(),
            .failed_secondary = failed.secondary.data(),
            .consumed = consumed.data(),
            .placement_capacity = destination.placements.capacity(),
            .migration_capacity = destination.migrations.capacity(),
            .failed_secondary_capacity = failed.secondary.capacity(),
            .consumed_capacity = consumed.capacity()};
}
auto PairwiseTypedAdapterStorageTestAccess::failed_primary(
    const PairwiseTypedAdapterStorage& storage, std::size_t block) noexcept
    -> const std::optional<PairwiseEvidenceFailure>& {
    return storage.destinations_.failed_observations[block].primary;
}
auto PairwiseTypedAdapterStorageTestAccess::failed_secondary(
    const PairwiseTypedAdapterStorage& storage, std::size_t block) noexcept
    -> std::span<const PairwiseEvidenceFailure> {
    return storage.destinations_.failed_observations[block].secondary;
}
auto PairwiseTypedAdapterStorageTestAccess::observation_capacities(
    std::size_t workers) noexcept
    -> std::optional<std::array<std::size_t, 2U>> {
    return checked_observation_capacities(workers);
}
auto PairwiseTypedAdapterStorageTestAccess::observation_worker_storage(
    const PairwiseTypedAdapterStorage& storage, std::size_t index) noexcept
    -> const void* {
    return std::visit([index](const auto& destinations) -> const void* {
        return destinations[index].data();
    }, storage.observation_workers_);
}
auto PairwiseTypedAdapterStorageTestAccess::observation_worker_capacity(
    const PairwiseTypedAdapterStorage& storage, std::size_t index) noexcept
    -> std::size_t {
    return std::visit([index](const auto& destinations) {
        return destinations[index].capacity();
    }, storage.observation_workers_);
}
PairwiseTypedPreparedOwners::PairwiseTypedPreparedOwners(
    PairwiseTypedAdapterStorage&& storage, PairwiseTypedOwnerOperations operations,
    std::array<bool, 4> acquired) noexcept
    : operations_{std::move(operations)}, storage_{std::move(storage)},
      acquired_{acquired}, ready_{acquired == std::array{true, true, true, true}} {
    if (ready_ && operations_.bind_prepared_owners != nullptr)
        operations_.bind_prepared_owners(operations_.context.get(), this);
}
PairwiseTypedPreparedOwners::PairwiseTypedPreparedOwners(
    PairwiseTypedPreparedOwners&& other) noexcept
    : operations_{std::move(other.operations_)},
      storage_{std::move(other.storage_)},
      acquired_{other.acquired_}, active_{std::exchange(other.active_, false)},
      ready_{std::exchange(other.ready_, false)} {
    other.acquired_.fill(false);
    if (ready_ && operations_.bind_prepared_owners != nullptr)
        operations_.bind_prepared_owners(operations_.context.get(), this);
}
void PairwiseTypedPreparedOwners::cleanup() noexcept {
    if (!active_)
        return;
    auto sink = storage_.failure_sink(
        PairwiseTypedFailureDestination::projected_cleanup);
    for (std::size_t ordinal = acquired_.size(); ordinal != 0U; --ordinal) {
        const auto index = ordinal - 1U;
        if (acquired_[index]) {
            operations_.cleanup(operations_.context.get(),
                static_cast<PairwiseTypedOwnerStage>(index), sink);
            acquired_[index] = false;
        }
    }
}
PairwiseTypedPreparedOwners::~PairwiseTypedPreparedOwners() noexcept { cleanup(); }
auto PairwiseTypedPreparedOwners::consume_observation_destination(
    const measurement::ScheduledObservationSlot& slot, void* context,
    ObservationDestinationCallback callback) noexcept -> bool {
    const auto block_value = slot.block_id().value();
    if (!active_ || !ready_ || callback == nullptr || operations_.admit_observation == nullptr ||
        !operations_.admit_observation(operations_.context.get(), slot) ||
        block_value > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::size_t>(block_value) >= storage_.blocks_ ||
        slot.slot_index() >= 4U)
        return false;
    const auto ordinal = static_cast<std::size_t>(block_value) * 4U +
                         slot.slot_index();
    if (ordinal >= storage_.factual_destinations_ ||
        storage_.destinations_.consumed_observation_ordinals[ordinal] != 0U)
        return false;
    storage_.destinations_.consumed_observation_ordinals[ordinal] = 1U;
    PairwiseTypedObservationDestination destination{storage_, ordinal};
    callback(context, destination);
    return true;
}
auto PairwiseTypedPreparedOwners::retire() && noexcept -> PairwiseTypedAdapterStorage {
    if (!active_)
        std::terminate();
    cleanup();
    active_ = false;
    return std::move(storage_);
}

PairwiseTypedReleasedOwnerPreparationFailure::
PairwiseTypedReleasedOwnerPreparationFailure(
    PairwiseEvidenceFailure primary,
    PairwiseTypedAdapterStorage&& storage) noexcept
    : primary_{std::move(primary)}, storage_{std::move(storage)} {}
PairwiseTypedReleasedOwnerPreparationFailure::
PairwiseTypedReleasedOwnerPreparationFailure(
    PairwiseTypedReleasedOwnerPreparationFailure&& other) noexcept
    : primary_{std::move(other.primary_)}, storage_{std::move(other.storage_)},
      active_{other.active_} {
    if (!active_) std::terminate();
    other.active_ = false;
}
void PairwiseTypedReleasedOwnerPreparationFailure::require_active() const noexcept {
    if (!active_) std::terminate();
}
auto PairwiseTypedReleasedOwnerPreparationFailure::primary() const noexcept
    -> const PairwiseEvidenceFailure& {
    require_active();
    return primary_;
}
auto PairwiseTypedReleasedOwnerPreparationFailure::storage() const noexcept
    -> const PairwiseTypedAdapterStorage& {
    require_active();
    return storage_;
}

PairwiseTypedOwnerPreparationFailure::PairwiseTypedOwnerPreparationFailure(
    PairwiseTypedOwnerPreparationFailure&& other) noexcept
    : primary_{std::move(other.primary_)}, storage_{std::move(other.storage_)},
      active_{other.active_} {
    if (!active_) std::terminate();
    other.active_ = false;
}
void PairwiseTypedOwnerPreparationFailure::require_active() const noexcept {
    if (!active_) std::terminate();
}
auto PairwiseTypedOwnerPreparationFailure::primary() const noexcept
    -> const PairwiseEvidenceFailure& {
    require_active();
    return primary_;
}
auto PairwiseTypedOwnerPreparationFailure::storage() const noexcept
    -> const PairwiseTypedAdapterStorage& {
    require_active();
    return storage_;
}
auto PairwiseTypedOwnerPreparationFailure::release() && noexcept
    -> PairwiseTypedReleasedOwnerPreparationFailure {
    require_active();
    PairwiseTypedReleasedOwnerPreparationFailure released{
        std::move(primary_), std::move(storage_)};
    active_ = false;
    return released;
}

auto prepare_pairwise_typed_owner_storage(PairwiseTypedAdapterStorage&& storage,
    PairwiseTypedOwnerOperations operations) noexcept
    -> std::expected<PairwiseTypedPreparedOwners,
                     PairwiseTypedOwnerPreparationFailure> {
    if (!operations.acquire || !operations.cleanup)
        return std::unexpected{PairwiseTypedOwnerPreparationFailure{
            preparation_failure(PairwiseFailureReason::contained_exception),
            std::move(storage)}};
    return PairwiseTypedPreparedOwners{std::move(storage), std::move(operations), {}};
}

auto acquire_pairwise_typed_owners(PairwiseTypedPreparedOwners&& input) noexcept
    -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure> {
    auto pending = std::move(input);
    if (!pending.active_ || pending.ready_) {
        if (pending.active_) pending.cleanup();
        pending.active_ = false;
        return std::unexpected{PairwiseTypedOwnerPreparationFailure{
            preparation_failure(PairwiseFailureReason::contained_exception),
            std::move(pending.storage_)}};
    }
    auto& storage = pending.storage_;
    auto& operations = pending.operations_;
    auto sink = storage.failure_sink(
        PairwiseTypedFailureDestination::preparation_secondary);
    std::array<bool, 4> acquired{};
    for (std::size_t ordinal = 0; ordinal < acquired.size(); ++ordinal) {
        auto result = operations.acquire(operations.context.get(),
            static_cast<PairwiseTypedOwnerStage>(ordinal), sink);
        if (result) { acquired[ordinal] = true; continue; }
        for (std::size_t cleanup_ordinal = ordinal; cleanup_ordinal != 0U;
             --cleanup_ordinal) {
            const auto index = cleanup_ordinal - 1U;
            operations.cleanup(operations.context.get(),
                static_cast<PairwiseTypedOwnerStage>(index), sink);
        }
        pending.active_ = false;
        return std::unexpected{PairwiseTypedOwnerPreparationFailure{
            std::move(result.error()), std::move(storage)}};
    }
    auto handoff = operations.acquire(operations.context.get(),
        PairwiseTypedOwnerStage::ownership_transfer, sink);
    if (!handoff) {
        for (std::size_t cleanup_ordinal = acquired.size(); cleanup_ordinal != 0U;
             --cleanup_ordinal) {
            const auto index = cleanup_ordinal - 1U;
            operations.cleanup(operations.context.get(),
                static_cast<PairwiseTypedOwnerStage>(index), sink);
        }
        pending.active_ = false;
        return std::unexpected{PairwiseTypedOwnerPreparationFailure{
            std::move(handoff.error()), std::move(storage)}};
    }
    pending.acquired_ = acquired;
    pending.ready_ = true;
    return pending;
}

auto prepare_pairwise_typed_owners(PairwiseTypedAdapterStorage&& storage,
    PairwiseTypedOwnerOperations operations) noexcept
    -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure> {
    auto pending = prepare_pairwise_typed_owner_storage(std::move(storage), std::move(operations));
    if (!pending) return std::unexpected{std::move(pending.error())};
    return acquire_pairwise_typed_owners(std::move(*pending));
}

} // namespace epycbench::runtime::detail
