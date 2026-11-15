#pragma once

#include "epycbench/runtime/pairwise_evidence_failure.hpp"
#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"
#include "runtime/pairwise_worker_owning_facade.hpp"
#include "runtime/pairwise_memory_owning_facade.hpp"
#include "runtime/pairwise_monitoring_owning_facade.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::runtime::detail {
class PairwiseProductionRunCore;
class PairwiseTypedReleasedOwnerPreparationFailure;
[[nodiscard]] auto materialize_pairwise_owner_preparation_failure(
    PairwiseTypedReleasedOwnerPreparationFailure&&) noexcept
    -> PairwiseOperationFailure;

class PairwiseTypedOwnedContext final {
  public:
    PairwiseTypedOwnedContext() noexcept = default;
    PairwiseTypedOwnedContext(PairwiseTypedOwnedContext&& other) noexcept
        : value_{std::exchange(other.value_, nullptr)},
          destroy_{std::exchange(other.destroy_, nullptr)} {}
    PairwiseTypedOwnedContext(const PairwiseTypedOwnedContext&) = delete;
    auto operator=(PairwiseTypedOwnedContext&&)
        -> PairwiseTypedOwnedContext& = delete;
    auto operator=(const PairwiseTypedOwnedContext&)
        -> PairwiseTypedOwnedContext& = delete;
    ~PairwiseTypedOwnedContext() noexcept {
        if (value_ != nullptr)
            destroy_(value_);
    }

    template <class T>
    [[nodiscard]] static auto from_unique(std::unique_ptr<T> value) noexcept
        -> PairwiseTypedOwnedContext {
        return PairwiseTypedOwnedContext{
            value.release(),
            [](void* opaque) noexcept { delete static_cast<T*>(opaque); }};
    }
    [[nodiscard]] auto get() const noexcept -> void* { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

  private:
    PairwiseTypedOwnedContext(void* value, void (*destroy)(void*) noexcept) noexcept
        : value_{value}, destroy_{destroy} {}
    void* value_{};
    void (*destroy_)(void*) noexcept{};
};

enum class PairwiseTypedFailureDestination : std::uint8_t {
    preparation_secondary,
    preparation_overflow,
    projected_cleanup,
    retained_lower,
    forced_team,
    finalization_local,
    global_operational,
    count,
};
struct PairwiseTypedOwnerOperations;
class PairwiseTypedPreparedOwners;
class PairwiseTypedOwnerPreparationFailure;
class PairwiseTypedProductionRunCore;
class PairwiseTypedPreOwnerPreparation;
struct PairwiseTypedAdapterStorageTestAccess;
class PairwiseTypedAdapterStorage;
class PairwiseTypedObservationDestination;
class PairwiseTypedReleasedOwnerPreparationFailure;
struct PairwiseTypedOperationFailure;

using PairwiseTypedPreparedObservationWorkers = std::variant<
    std::vector<std::vector<measurement::SequentialReadBandwidthWorkerObservation>>,
    std::vector<std::vector<measurement::DependentLoadLatencyWorkerObservation>>>;

// A synchronous, fixed-destination capability used only by the private owner
// composition. It cannot be constructed, copied, moved, or rebound by a lower
// owner callback.
class PairwiseTypedFailureSink final {
  public:
    PairwiseTypedFailureSink(const PairwiseTypedFailureSink&) = delete;
    PairwiseTypedFailureSink(PairwiseTypedFailureSink&&) = delete;
    auto operator=(const PairwiseTypedFailureSink&)
        -> PairwiseTypedFailureSink& = delete;
    auto operator=(PairwiseTypedFailureSink&&)
        -> PairwiseTypedFailureSink& = delete;
    [[nodiscard]] auto append(PairwiseEvidenceFailure) noexcept -> bool;

  private:
    PairwiseTypedFailureSink(PairwiseTypedAdapterStorage&,
                             PairwiseTypedFailureDestination) noexcept;
    PairwiseTypedAdapterStorage* storage_{};
    const PairwiseTypedFailureDestination destination_;
    friend class PairwiseTypedAdapterStorage;
};

// Inactive adapter-side storage. Every allocation occurs in create(), before
// any lower owner is accepted by the future typed adapter composition.
class PairwiseTypedAdapterStorage final {
  public:
    PairwiseTypedAdapterStorage(PairwiseTypedAdapterStorage&&) noexcept = default;
    PairwiseTypedAdapterStorage(const PairwiseTypedAdapterStorage&) = delete;
    auto operator=(PairwiseTypedAdapterStorage&&) noexcept
        -> PairwiseTypedAdapterStorage& = default;
    auto operator=(const PairwiseTypedAdapterStorage&)
        -> PairwiseTypedAdapterStorage& = delete;

    [[nodiscard]] static auto create(std::size_t workers, std::size_t maximum_blocks) noexcept
        -> std::expected<PairwiseTypedAdapterStorage, PairwiseEvidenceFailure>;
    void install_observation_workers(
        PairwiseTypedPreparedObservationWorkers&&) noexcept;
    [[nodiscard]] auto append(PairwiseTypedFailureDestination,
                              PairwiseEvidenceFailure) noexcept -> bool;
    [[nodiscard]] auto consume_worker_callback(
        const PairwiseWorkerCallbackFailure&,
        PairwiseTypedFailureDestination) noexcept -> bool;
    [[nodiscard]] auto consume_memory_finalization(
        const PairwiseMemoryFinalizationFailure&,
        PairwiseTypedFailureDestination) noexcept -> bool;
    [[nodiscard]] auto consume_memory_sequence(
        const PairwiseMemoryFailureSequence&,
        PairwiseTypedFailureDestination) noexcept -> bool;
    [[nodiscard]] auto consume_monitoring_operation(
        const PairwiseMonitoringOperationFailure&,
        PairwiseTypedFailureDestination) noexcept -> bool;
    [[nodiscard]] auto append_effect(statistics::PairedLogEffect) noexcept -> bool;
    [[nodiscard]] auto effect_count() const noexcept -> std::size_t;
    [[nodiscard]] auto placement_count(std::size_t) const noexcept -> std::size_t;
    [[nodiscard]] auto migration_count(std::size_t) const noexcept -> std::size_t;
    [[nodiscard]] auto failed_observation_secondary_count(std::size_t) const noexcept
        -> std::size_t;
    [[nodiscard]] auto failed_observation_secondary_capacity(std::size_t) const noexcept
        -> std::size_t;
#define EPYCBENCH_TYPED_CONSUMER(name, type)                                      \
    [[nodiscard]] auto consume_##name(const type&,                               \
        PairwiseTypedFailureDestination) noexcept -> bool;
    EPYCBENCH_TYPED_CONSUMER(platform, PairwisePlatformFailure)
    EPYCBENCH_TYPED_CONSUMER(memory, PairwiseMemoryFailure)
    EPYCBENCH_TYPED_CONSUMER(monitoring, PairwiseMonitoringFailure)
    EPYCBENCH_TYPED_CONSUMER(kernel, PairwiseKernelFailure)
    EPYCBENCH_TYPED_CONSUMER(measurement, PairwiseMeasurementFailure)
    EPYCBENCH_TYPED_CONSUMER(statistics, PairwiseStatisticsFailure)
    EPYCBENCH_TYPED_CONSUMER(worker, PairwiseWorkerFailure)
    EPYCBENCH_TYPED_CONSUMER(runtime, PairwiseRuntimeFailure)
#undef EPYCBENCH_TYPED_CONSUMER
    [[nodiscard]] auto values(PairwiseTypedFailureDestination) const noexcept
        -> std::span<const PairwiseEvidenceFailure>;
    [[nodiscard]] auto capacity(PairwiseTypedFailureDestination) const noexcept
        -> std::size_t;
    [[nodiscard]] auto workers() const noexcept -> std::size_t { return workers_; }
    [[nodiscard]] auto maximum_blocks() const noexcept -> std::size_t { return blocks_; }
    [[nodiscard]] auto factual_destination_count() const noexcept -> std::size_t {
        return factual_destinations_;
    }
    [[nodiscard]] auto factual_worker_capacity() const noexcept -> std::size_t {
        return workers_;
    }
    [[nodiscard]] auto finalization_association_capacity() const noexcept -> std::size_t {
        return finalization_associations_;
    }
    [[nodiscard]] auto cleanup_overflow() const noexcept -> bool {
        return cleanup_overflow_;
    }
    [[nodiscard]] auto cleanup_overflow_invariant() const noexcept
        -> const PairwiseEvidenceFailure& { return cleanup_overflow_invariant_; }
    [[nodiscard]] auto first_cleanup_overflow() const noexcept
        -> const std::optional<PairwiseEvidenceFailure>& {
        return first_cleanup_overflow_;
    }

  private:
    struct ObservationDestination {
        std::vector<measurement::WorkerPlacementValidityEvidence> placements;
        std::vector<measurement::WorkerMigrationValidityEvidence> migrations;
    };
    struct FailedObservationDestination {
        std::optional<PairwiseEvidenceFailure> primary;
        std::vector<PairwiseEvidenceFailure> secondary;
    };
    struct BlockDestination {
        std::optional<measurement::MeasurementBlockPlan> plan;
        std::optional<PairwiseEvidenceFailure> operational_failure;
    };
    struct DestinationWorkspace {
        std::vector<std::optional<BlockDestination>> blocks;
        std::vector<std::optional<RunnerStoppingEvaluationEvidence>> stopping;
        std::vector<std::optional<statistics::PairedLogEffect>> effects;
        std::vector<ObservationDestination> observations;
        std::vector<FailedObservationDestination> failed_observations;
        std::vector<std::uint8_t> consumed_observation_ordinals;
        std::vector<LocalFinalizationErrorAssociation> finalization_associations;
    };
    explicit PairwiseTypedAdapterStorage(std::size_t, std::size_t,
        std::array<std::vector<PairwiseEvidenceFailure>,
                   static_cast<std::size_t>(PairwiseTypedFailureDestination::count)>&&,
        DestinationWorkspace&&) noexcept;
    std::size_t workers_{};
    std::size_t blocks_{};
    std::size_t factual_destinations_{};
    std::size_t finalization_associations_{};
    std::array<std::vector<PairwiseEvidenceFailure>,
               static_cast<std::size_t>(PairwiseTypedFailureDestination::count)> storage_;
    DestinationWorkspace destinations_;
    PairwiseEvidenceFailure cleanup_overflow_invariant_;
    std::optional<PairwiseEvidenceFailure> first_cleanup_overflow_;
    PairwiseTypedPreparedObservationWorkers observation_workers_;
    bool observation_workers_installed_{};
    bool cleanup_overflow_{};
    [[nodiscard]] auto failure_sink(PairwiseTypedFailureDestination) noexcept
        -> PairwiseTypedFailureSink;
    void append_cleanup_or_mark(PairwiseTypedFailureDestination,
                                PairwiseEvidenceFailure&&) noexcept;
    friend class PairwiseTypedFailureSink;
    friend class PairwiseTypedPreparedOwners;
    friend auto materialize_pairwise_owner_preparation_failure(
        PairwiseTypedReleasedOwnerPreparationFailure&&) noexcept
        -> PairwiseOperationFailure;
    friend auto acquire_pairwise_typed_owners(PairwiseTypedPreparedOwners&&) noexcept
        -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
    friend auto prepare_pairwise_typed_owners(PairwiseTypedAdapterStorage&&,
        PairwiseTypedOwnerOperations) noexcept
        -> std::expected<PairwiseTypedPreparedOwners,
                         PairwiseTypedOwnerPreparationFailure>;
    friend class PairwiseProductionRunCore;
    friend class PairwiseTypedObservationDestination;
    friend class PairwiseTypedPreparedOwners;
    friend struct PairwiseTypedAdapterStorageTestAccess;
};

// A synchronous, ordinal-specific view. Only PairwiseTypedPreparedOwners may
// mint it, and it cannot survive the callback in which it is supplied.
class PairwiseTypedObservationDestination final {
  public:
    PairwiseTypedObservationDestination(const PairwiseTypedObservationDestination&) = delete;
    PairwiseTypedObservationDestination(PairwiseTypedObservationDestination&&) = delete;
    auto operator=(const PairwiseTypedObservationDestination&)
        -> PairwiseTypedObservationDestination& = delete;
    auto operator=(PairwiseTypedObservationDestination&&)
        -> PairwiseTypedObservationDestination& = delete;
    [[nodiscard]] auto append_placement(
        measurement::WorkerPlacementValidityEvidence) noexcept -> bool;
    [[nodiscard]] auto append_migration(
        measurement::WorkerMigrationValidityEvidence) noexcept -> bool;
    [[nodiscard]] auto set_failed_primary(PairwiseEvidenceFailure) noexcept -> bool;
    [[nodiscard]] auto append_failed_secondary(PairwiseEvidenceFailure) noexcept -> bool;
    [[nodiscard]] auto take_placements() noexcept
        -> std::vector<measurement::WorkerPlacementValidityEvidence>;
    [[nodiscard]] auto take_migrations() noexcept
        -> std::vector<measurement::WorkerMigrationValidityEvidence>;
    [[nodiscard]] auto append_bandwidth_worker(
        measurement::SequentialReadBandwidthWorkerObservation) noexcept -> bool;
    [[nodiscard]] auto append_latency_worker(
        measurement::DependentLoadLatencyWorkerObservation) noexcept -> bool;
    [[nodiscard]] auto take_bandwidth_workers() noexcept
        -> std::vector<measurement::SequentialReadBandwidthWorkerObservation>;
    [[nodiscard]] auto take_latency_workers() noexcept
        -> std::vector<measurement::DependentLoadLatencyWorkerObservation>;
  private:
    PairwiseTypedObservationDestination(PairwiseTypedAdapterStorage& storage,
                                        std::size_t ordinal) noexcept
        : storage_{&storage}, ordinal_{ordinal} {}
    PairwiseTypedAdapterStorage* storage_{};
    const std::size_t ordinal_{};
    friend class PairwiseTypedPreparedOwners;
};

struct PairwiseTypedAdapterStorageTestAccess final {
    struct ObservationFacts final {
        const void* placement{};
        const void* migration{};
        const void* failed_secondary{};
        const void* consumed{};
        std::size_t placement_capacity{};
        std::size_t migration_capacity{};
        std::size_t failed_secondary_capacity{};
        std::size_t consumed_capacity{};
    };
    static void install_allocation_checkpoint(void* context,
        void (*checkpoint)(void*, std::size_t, bool)) noexcept;
    static void clear_allocation_checkpoint() noexcept;
    [[nodiscard]] static auto observation_facts(
        const PairwiseTypedAdapterStorage&, std::size_t, std::size_t) noexcept
        -> ObservationFacts;
    [[nodiscard]] static auto failed_primary(
        const PairwiseTypedAdapterStorage&, std::size_t) noexcept
        -> const std::optional<PairwiseEvidenceFailure>&;
    [[nodiscard]] static auto failed_secondary(
        const PairwiseTypedAdapterStorage&, std::size_t) noexcept
        -> std::span<const PairwiseEvidenceFailure>;
    [[nodiscard]] static auto observation_capacities(std::size_t) noexcept
        -> std::optional<std::array<std::size_t, 2U>>;
    [[nodiscard]] static auto observation_worker_storage(
        const PairwiseTypedAdapterStorage&, std::size_t) noexcept -> const void*;
    [[nodiscard]] static auto observation_worker_capacity(
        const PairwiseTypedAdapterStorage&, std::size_t) noexcept -> std::size_t;
};

enum class PairwiseTypedOwnerStage : std::uint8_t {
    worker_team = 0U,
    first_memory = 1U,
    second_memory = 2U,
    monitoring = 3U,
    ownership_transfer = 4U,
    count,
};
static_assert(static_cast<std::uint8_t>(PairwiseTypedOwnerStage::worker_team) == 0U);
static_assert(static_cast<std::uint8_t>(PairwiseTypedOwnerStage::first_memory) == 1U);
static_assert(static_cast<std::uint8_t>(PairwiseTypedOwnerStage::second_memory) == 2U);
static_assert(static_cast<std::uint8_t>(PairwiseTypedOwnerStage::monitoring) == 3U);
static_assert(static_cast<std::uint8_t>(PairwiseTypedOwnerStage::ownership_transfer) == 4U);
static_assert(static_cast<std::uint8_t>(PairwiseTypedOwnerStage::count) == 5U);
struct PairwiseTypedOwnerOperations final {
    PairwiseTypedOwnedContext context;
    std::expected<void, PairwiseEvidenceFailure> (*acquire)(
        void*, PairwiseTypedOwnerStage, PairwiseTypedFailureSink&) noexcept{};
    void (*cleanup)(void*, PairwiseTypedOwnerStage,
                    PairwiseTypedFailureSink&) noexcept{};
    bool (*admit_observation)(
        void*, const measurement::ScheduledObservationSlot&) noexcept{};
    void (*bind_prepared_owners)(void*, PairwiseTypedPreparedOwners*) noexcept{};
};
class PairwiseTypedPreparedOwners final {
  public:
    PairwiseTypedPreparedOwners(PairwiseTypedPreparedOwners&&) noexcept;
    PairwiseTypedPreparedOwners(const PairwiseTypedPreparedOwners&) = delete;
    auto operator=(PairwiseTypedPreparedOwners&&) -> PairwiseTypedPreparedOwners& = delete;
    ~PairwiseTypedPreparedOwners() noexcept;
    [[nodiscard]] auto storage() const noexcept
        -> const PairwiseTypedAdapterStorage& { return storage_; }
    [[nodiscard]] auto context() const noexcept -> void* {
        return active_ && ready_ ? operations_.context.get() : nullptr;
    }
    [[nodiscard]] auto retire() && noexcept -> PairwiseTypedAdapterStorage;
    using ObservationDestinationCallback = void (*)(
        void*, PairwiseTypedObservationDestination&) noexcept;
    [[nodiscard]] auto consume_observation_destination(
        const measurement::ScheduledObservationSlot& slot, void* context,
        ObservationDestinationCallback callback) noexcept -> bool;
  private:
    PairwiseTypedPreparedOwners(PairwiseTypedAdapterStorage&&, PairwiseTypedOwnerOperations,
                                std::array<bool, 4>) noexcept;
    void cleanup() noexcept;
    PairwiseTypedOwnerOperations operations_;
    PairwiseTypedAdapterStorage storage_;
    std::array<bool, 4> acquired_{};
    bool active_{true};
    bool ready_{};
    friend auto prepare_pairwise_typed_owner_storage(
        PairwiseTypedAdapterStorage&&, PairwiseTypedOwnerOperations) noexcept
        -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
    friend auto acquire_pairwise_typed_owners(PairwiseTypedPreparedOwners&&) noexcept
        -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
    friend auto prepare_pairwise_typed_owners(PairwiseTypedAdapterStorage&&,
        PairwiseTypedOwnerOperations) noexcept
        -> std::expected<PairwiseTypedPreparedOwners,
                         PairwiseTypedOwnerPreparationFailure>;
    friend class PairwiseProductionRunCore;
};
class PairwiseTypedReleasedOwnerPreparationFailure final {
  public:
    PairwiseTypedReleasedOwnerPreparationFailure(
        PairwiseTypedReleasedOwnerPreparationFailure&&) noexcept;
    PairwiseTypedReleasedOwnerPreparationFailure(
        const PairwiseTypedReleasedOwnerPreparationFailure&) = delete;
    auto operator=(PairwiseTypedReleasedOwnerPreparationFailure&&)
        -> PairwiseTypedReleasedOwnerPreparationFailure& = delete;
    auto operator=(const PairwiseTypedReleasedOwnerPreparationFailure&)
        -> PairwiseTypedReleasedOwnerPreparationFailure& = delete;
    [[nodiscard]] auto primary() const noexcept
        -> const PairwiseEvidenceFailure&;
    [[nodiscard]] auto storage() const noexcept
        -> const PairwiseTypedAdapterStorage&;

  private:
    PairwiseTypedReleasedOwnerPreparationFailure(
        PairwiseEvidenceFailure, PairwiseTypedAdapterStorage&&) noexcept;
    void require_active() const noexcept;
    PairwiseEvidenceFailure primary_;
    PairwiseTypedAdapterStorage storage_;
    bool active_{true};
    friend class PairwiseTypedOwnerPreparationFailure;
    friend auto materialize_pairwise_owner_preparation_failure(
        PairwiseTypedReleasedOwnerPreparationFailure&&) noexcept
        -> PairwiseOperationFailure;
};
class PairwiseTypedOwnerPreparationFailure final {
  public:
    PairwiseTypedOwnerPreparationFailure(
        PairwiseTypedOwnerPreparationFailure&&) noexcept;
    PairwiseTypedOwnerPreparationFailure(const PairwiseTypedOwnerPreparationFailure&) = delete;
    auto operator=(PairwiseTypedOwnerPreparationFailure&&)
        -> PairwiseTypedOwnerPreparationFailure& = delete;
    auto operator=(const PairwiseTypedOwnerPreparationFailure&)
        -> PairwiseTypedOwnerPreparationFailure& = delete;
    [[nodiscard]] auto primary() const noexcept -> const PairwiseEvidenceFailure&;
    [[nodiscard]] auto storage() const noexcept -> const PairwiseTypedAdapterStorage&;
    [[nodiscard]] auto release() && noexcept
        -> PairwiseTypedReleasedOwnerPreparationFailure;
  private:
    PairwiseTypedOwnerPreparationFailure(PairwiseEvidenceFailure primary,
        PairwiseTypedAdapterStorage&& storage) noexcept
        : primary_{std::move(primary)}, storage_{std::move(storage)} {}
    void require_active() const noexcept;
    PairwiseEvidenceFailure primary_;
    PairwiseTypedAdapterStorage storage_;
    bool active_{true};
    friend auto prepare_pairwise_typed_owner_storage(
        PairwiseTypedAdapterStorage&&, PairwiseTypedOwnerOperations) noexcept
        -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
    friend auto acquire_pairwise_typed_owners(PairwiseTypedPreparedOwners&&) noexcept
        -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
    friend auto prepare_pairwise_typed_owners(PairwiseTypedAdapterStorage&&,
        PairwiseTypedOwnerOperations) noexcept
        -> std::expected<PairwiseTypedPreparedOwners,
                         PairwiseTypedOwnerPreparationFailure>;
};
// Allocation-only envelope followed by one explicit resource-acquisition step.
[[nodiscard]] auto prepare_pairwise_typed_owner_storage(
    PairwiseTypedAdapterStorage&&, PairwiseTypedOwnerOperations) noexcept
    -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
[[nodiscard]] auto acquire_pairwise_typed_owners(PairwiseTypedPreparedOwners&&) noexcept
    -> std::expected<PairwiseTypedPreparedOwners, PairwiseTypedOwnerPreparationFailure>;
[[nodiscard]] auto prepare_pairwise_typed_owners(PairwiseTypedAdapterStorage&&,
    PairwiseTypedOwnerOperations) noexcept
    -> std::expected<PairwiseTypedPreparedOwners,
                     PairwiseTypedOwnerPreparationFailure>;

} // namespace epycbench::runtime::detail
