#pragma once

#include "runtime/stage_b_resource_policy.hpp"

#include "runtime/pairwise_monitoring_owning_facade.hpp"
#include "runtime/pairwise_memory_owning_facade.hpp"
#include "runtime/pairwise_prepared_typed_measurement_execution.hpp"
#include "runtime/pairwise_typed_adapter_storage.hpp"
#include "runtime/pairwise_typed_local_monitoring_capacity_producer.hpp"
#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"
#include "runtime/pairwise_worker_owning_facade.hpp"
#include "epycbench/measurement/block_scheduler.hpp"
#include "epycbench/measurement/validity.hpp"

#include <memory>
#include "runtime/pairwise_preparation_graph.hpp"
#include <optional>
#include <variant>
#include <vector>

namespace epycbench::runtime::detail {

class PairwiseTypedOwnerTransaction;
class PairwiseProgressState;
class PairwisePreparationGraph;
class PairwiseProductionRunCore;
struct PairwiseFinalizationSource;
struct PairwiseTypedOwnerTransactionTestAccess;

enum class PairwiseTypedPreparedFamily : std::uint8_t { bandwidth, latency };

using PairwisePreparedTypedMeasurement = std::variant<
    PairwisePreparedBandwidthMeasurementExecution,
    PairwisePreparedLatencyMeasurementExecution>;

using PairwisePreparedObservationWorkers = PairwiseTypedPreparedObservationWorkers;

struct PairwisePreparedMemoryInput final {
    memory::RegionRequest request;
    PairwiseMemoryWorkspace workspace;
};

// Shared allocation-only projection; neither helper acquires native owners.
[[nodiscard]] auto prepare_pairwise_worker_specs(const AdmittedPlacementRequest&)
    -> std::vector<WorkerLaunchSpec>;
[[nodiscard]] auto prepare_pairwise_memory_inputs(
    const AdmittedPlacementRequest&, const PairwiseMemoryOwningFacade&)
    -> std::expected<std::vector<PairwisePreparedMemoryInput>, PairwiseEvidenceFailure>;

struct PairwisePreparedMemoryOwnerDestination final {
    std::optional<memory::ReadOnlyRegion> owner;
    std::optional<kernel::PreparedSequentialReadBandwidth> bandwidth;
    std::optional<kernel::PreparedDependentLoadLatency> latency;
    std::optional<platform_linux::RawMonotonicTimestamp> last_start;
    std::optional<platform_linux::RawMonotonicTimestamp> last_end;
    std::uint64_t last_work_units{};
    std::uint64_t last_logical_bytes{};
    std::uint64_t last_expected_terminal{};
    std::uint64_t last_actual_terminal{};
    bool last_verification_matched{};
    std::optional<kernel::SequentialReadBandwidthVerification>
        last_bandwidth_verification;
    std::optional<kernel::SequentialReadBandwidthWork> last_bandwidth_work;
    std::optional<kernel::DependentLoadLatencyVerification>
        last_latency_verification;
    std::optional<kernel::DependentLoadLatencySchedule> last_latency_schedule;
    std::optional<PairwiseEvidenceFailure> deferred_cleanup;
    std::optional<PairwiseEvidenceFailure> deferred_cleanup_tail;
};

struct PairwisePreparedFinalizationSlot final {
    std::optional<memory::RegionFinalizationDiagnostic> diagnostic;
    std::optional<std::uint64_t> primary_error_ordinal;
    std::optional<std::uint64_t> cleanup_error_ordinal;
};

struct PairwiseTypedPreOwnerDependencies final {
    PairwiseTypedPreOwnerDependencies(
        const PairwiseTypedLocalMonitoringCapacityProducer* capacity,
        PairwisePlatformLeafFacade platform_value,
        platform_linux::detail::BoundedSysfsOperations temperature,
        platform_linux::detail::BoundedSysfsOperations frequency,
        PairwiseKernelLeafFacade kernel_value = {},
        std::optional<PairwiseWorkerOwningDependencies> worker_value = std::nullopt,
        std::shared_ptr<RunnerCancellation> shared_cancellation_value = {},
        stage_b_policy::Selection policy_value = stage_b_policy::Selection::existing_caller)
        : capacity_producer{capacity}, platform{std::move(platform_value)},
          temperature_operations{temperature}, frequency_operations{frequency},
          kernel{std::move(kernel_value)}, worker{std::move(worker_value)},
          shared_cancellation{std::move(shared_cancellation_value)}, policy{policy_value} {}
    const PairwiseTypedLocalMonitoringCapacityProducer* capacity_producer{};
    PairwisePlatformLeafFacade platform;
    platform_linux::detail::BoundedSysfsOperations temperature_operations{};
    platform_linux::detail::BoundedSysfsOperations frequency_operations{};
    PairwiseKernelLeafFacade kernel{};
    std::optional<PairwiseWorkerOwningDependencies> worker{};
    std::shared_ptr<RunnerCancellation> shared_cancellation{};
    stage_b_policy::Selection policy{stage_b_policy::Selection::existing_caller};
};

// Owned, immutable dependency bundle for the mechanically unselected real
// typed binding.  In particular, the capacity producer is retained by the
// factory context rather than borrowed across prepare_run().
struct PairwiseTypedProductionBindingDependencies final {
    PairwiseTypedLocalMonitoringCapacityProducer capacity_producer;
    PairwisePlatformLeafFacade platform;
    platform_linux::detail::BoundedSysfsOperations temperature_operations{};
    platform_linux::detail::BoundedSysfsOperations frequency_operations{};
    PairwiseKernelLeafFacade kernel{};
    std::optional<PairwiseWorkerOwningDependencies> worker{};
    std::shared_ptr<RunnerCancellation> shared_cancellation{};
    stage_b_policy::Selection policy{stage_b_policy::Selection::existing_caller};
};

// Private opt-in observer aliases the binding context; no additional receipt owner.
struct PairwisePreparationBindingBundle final {
    PairwiseProductionOperations operations;
    std::shared_ptr<const std::optional<PairwisePreparationGraph>> observer;
    [[nodiscard]] auto view() const noexcept -> const PairwisePreparationGraph* {
        return observer && *observer && (*observer)->stable() ? &**observer : nullptr;
    }
};
[[nodiscard]] auto make_pairwise_preparation_binding(
    PairwiseTypedProductionBindingDependencies, PreparationGraphLimits) noexcept
    -> PairwiseOperationResult<PairwisePreparationBindingBundle>;

// Complete allocation-only prefix of the future real typed binding.  None of
// these values owns an OS resource; the first active owner is installed only
// by the subsequent owner-acquisition slice.
class PairwiseTypedPreOwnerPreparation final {
  public:
    PairwiseTypedPreOwnerPreparation(PairwiseTypedPreOwnerPreparation&&) noexcept;
    PairwiseTypedPreOwnerPreparation(const PairwiseTypedPreOwnerPreparation&) = delete;
    auto operator=(PairwiseTypedPreOwnerPreparation&&)
        -> PairwiseTypedPreOwnerPreparation& = delete;
    auto operator=(const PairwiseTypedPreOwnerPreparation&)
        -> PairwiseTypedPreOwnerPreparation& = delete;

  private:
    PairwiseTypedPreOwnerPreparation(
        std::unique_ptr<RunnerCancellation>, std::vector<WorkerLaunchSpec>,
        measurement::CalibrationDurationPolicy, measurement::WarmupPolicy,
        measurement::RepetitionPolicy, PairwisePreparedTypedMeasurement,
        PairwiseTypedAdapterStorage, PairwisePreparedWorkerTeam,
        PairwisePreparedMonitoringStorage,
        std::vector<PairwisePreparedMemoryInput>,
        std::vector<PairwisePreparedMemoryInput>,
        std::vector<PairwisePreparedMemoryOwnerDestination>,
        std::vector<PairwisePreparedMemoryOwnerDestination>,
        PairwisePlatformLeafFacade, PairwiseKernelLeafFacade,
        PairwiseWorkerOwningDependencies,
        PairwisePreparedRegionInitializer, ipc::RequestId, ipc::RequestId,
        AdmittedPlacementIdentity, AdmittedPlacementIdentity,
        PairwiseTypedPreparedFamily, std::uint64_t,
        monitoring::MonitoringExecutionPolicy, ServicePlacementPlan,
        StatisticsProductionPolicy, measurement::ComparisonOrderPolicy,
        measurement::ObservationValidityPolicy, bool, std::size_t,
        std::optional<kernel::SequentialReadBandwidthConfig>,
        std::optional<kernel::DependentLoadLatencyConfig>,
        std::optional<kernel::PreparedDependentLoadLatencyVerification>,
        kernel::KernelImplementationId, PairwisePreparedObservationWorkers,
        std::uint64_t, std::uint64_t, core::ExperimentId,
        kernel::DependentLoadLatencyBlockId,
        std::vector<PairwiseEvidenceFailure>,
        std::vector<PairwisePreparedFinalizationSlot>,
        std::vector<PairwisePreparedFinalizationSlot>,
        std::vector<FinalizedWorkerEvidence>,
        std::vector<FinalizedWorkerEvidence>,
        std::vector<LocalFinalizationErrorAssociation>) noexcept;

    std::unique_ptr<RunnerCancellation> cancellation_;
    std::shared_ptr<RunnerCancellation> shared_cancellation_;
    std::vector<WorkerLaunchSpec> worker_specs_;
    measurement::CalibrationDurationPolicy calibration_;
    measurement::WarmupPolicy warmup_;
    measurement::RepetitionPolicy repetition_;
    measurement::ComparisonOrderPolicy comparison_order_;
    measurement::ObservationValidityPolicy validity_;
    bool calibration_enabled_{};
    PairwisePreparedTypedMeasurement measurement_;
    PairwiseTypedAdapterStorage adapter_;
    PairwisePreparedWorkerTeam worker_team_;
    PairwisePreparedMonitoringStorage monitoring_;
    std::vector<PairwisePreparedMemoryInput> first_memory_;
    std::vector<PairwisePreparedMemoryInput> second_memory_;
    std::vector<PairwisePreparedMemoryOwnerDestination> first_memory_owners_;
    std::vector<PairwisePreparedMemoryOwnerDestination> second_memory_owners_;
    PairwisePreparedRegionInitializer initializer_;
    PairwisePlatformLeafFacade platform_;
    PairwiseKernelLeafFacade kernel_;
    PairwiseWorkerOwningDependencies worker_dependencies_;
    ipc::RequestId first_request_;
    ipc::RequestId second_request_;
    AdmittedPlacementIdentity first_identity_;
    AdmittedPlacementIdentity second_identity_;
    PairwiseTypedPreparedFamily family_;
    std::uint64_t comparison_order_seed_{};
    monitoring::MonitoringExecutionPolicy monitoring_policy_;
    ServicePlacementPlan service_placement_;
    StatisticsProductionPolicy statistics_policy_;
    std::size_t maximum_blocks_{};
    std::optional<kernel::SequentialReadBandwidthConfig> bandwidth_configuration_;
    std::optional<kernel::DependentLoadLatencyConfig> latency_configuration_;
    std::optional<kernel::PreparedDependentLoadLatencyVerification>
        latency_verification_;
    kernel::KernelImplementationId implementation_id_{};
    PairwisePreparedObservationWorkers observation_workers_;
    std::uint64_t resolved_latency_seed_{};
    std::uint64_t measurement_root_seed_{};
    core::ExperimentId calibration_experiment_id_{0U};
    kernel::DependentLoadLatencyBlockId calibration_block_id_{0U};
    std::vector<PairwiseEvidenceFailure> operation_secondary_;
    std::vector<PairwisePreparedFinalizationSlot> first_finalization_slots_;
    std::vector<PairwisePreparedFinalizationSlot> second_finalization_slots_;
    std::vector<FinalizedWorkerEvidence> first_finalized_workers_;
    std::vector<FinalizedWorkerEvidence> second_finalized_workers_;
    std::vector<LocalFinalizationErrorAssociation> finalization_associations_;
    bool active_{true};
    std::optional<PairwisePreparationGraph> preparation_graph_;
    friend class PairwisePreparationGraph;
    friend auto prepare_pairwise_typed_pre_owner(
        const AdmittedPairwiseProductionInputs&,
        const PairwiseTypedPreOwnerDependencies&, std::optional<PreparationGraphLimits>) noexcept
        -> PairwiseOperationResult<PairwiseTypedPreOwnerPreparation>;
    friend auto prepare_pairwise_typed_owner_transaction_storage(
        PairwiseTypedPreOwnerPreparation&&, PairwiseProgressState*) noexcept
        -> PairwiseOperationResult<PairwiseTypedPreparedOwners>;
    friend auto prepare_pairwise_typed_owner_transaction(
        PairwiseTypedPreOwnerPreparation&&) noexcept
        -> PairwiseOperationResult<PairwiseTypedPreparedOwners>;
    friend class PairwiseTypedOwnerTransaction;
    friend class PairwiseTypedProductionBinding;
    friend class PreparedPairwiseRunnerExecution;
    friend struct PairwiseTypedPreOwnerPreparationTestAccess;
};

struct PairwiseTypedPreOwnerPreparationTestAccess final {
    [[nodiscard]] static auto worker_count(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return value.worker_specs_.size();
    }
    [[nodiscard]] static auto maximum_blocks(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return value.maximum_blocks_;
    }
    [[nodiscard]] static auto worker_specs(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> std::span<const WorkerLaunchSpec> { return value.worker_specs_; }
    [[nodiscard]] static auto family(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.family_;
    }
    [[nodiscard]] static auto active(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.active_;
    }
    [[nodiscard]] static auto comparison_order_seed(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.comparison_order_seed_;
    }
    [[nodiscard]] static auto monitoring_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const monitoring::MonitoringExecutionPolicy& {
        return value.monitoring_policy_;
    }
    [[nodiscard]] static auto service_placement(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const ServicePlacementPlan& { return value.service_placement_; }
    [[nodiscard]] static auto statistics_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const StatisticsProductionPolicy& { return value.statistics_policy_; }
    [[nodiscard]] static auto calibration_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const measurement::CalibrationDurationPolicy& { return value.calibration_; }
    [[nodiscard]] static auto warmup_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const measurement::WarmupPolicy& { return value.warmup_; }
    [[nodiscard]] static auto repetition_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const measurement::RepetitionPolicy& { return value.repetition_; }
    [[nodiscard]] static auto comparison_order_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const measurement::ComparisonOrderPolicy& {
        return value.comparison_order_;
    }
    [[nodiscard]] static auto validity_policy(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const measurement::ObservationValidityPolicy& { return value.validity_; }
    [[nodiscard]] static auto calibration_enabled(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.calibration_enabled_;
    }
    [[nodiscard]] static auto measurement_index(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return value.measurement_.index();
    }
    [[nodiscard]] static auto measurement_pointers(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return std::visit([](const auto& prepared) {
            using Access = PairwisePreparedMeasurementExecutionTestAccess;
            return std::array<const void*, 4U>{
                Access::empty_data(prepared), Access::candidate_data(prepared),
                Access::warmup_data(prepared), Access::scratch_data(prepared)};
        }, value.measurement_);
    }
    [[nodiscard]] static auto measurement_capacities(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return std::visit([](const auto& prepared) {
            return PairwisePreparedMeasurementExecutionTestAccess::capacities(prepared);
        }, value.measurement_);
    }
    [[nodiscard]] static auto adapter_capacities(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        std::array<std::size_t,
                   static_cast<std::size_t>(PairwiseTypedFailureDestination::count)>
            result{};
        for (std::size_t index{}; index < result.size(); ++index)
            result[index] = value.adapter_.capacity(
                static_cast<PairwiseTypedFailureDestination>(index));
        return result;
    }
    [[nodiscard]] static auto adapter_factual_destination_count(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.adapter_.factual_destination_count();
    }
    [[nodiscard]] static auto adapter_factual_worker_capacity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.adapter_.factual_worker_capacity();
    }
    [[nodiscard]] static auto adapter_finalization_association_capacity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.adapter_.finalization_association_capacity();
    }
    [[nodiscard]] static auto worker_generation_capacity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return value.worker_team_.generation_capacity();
    }
    [[nodiscard]] static auto monitoring_storage_identity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return PairwiseMonitoringPreparationTestAccess::storage_identity(
            value.monitoring_);
    }
    [[nodiscard]] static auto first_memory_count(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return value.first_memory_.size();
    }
    [[nodiscard]] static auto second_memory_count(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return value.second_memory_.size();
    }
    [[nodiscard]] static auto first_memory_page_capacity(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept {
        return value.first_memory_[index].workspace.batch_capacity();
    }
    [[nodiscard]] static auto second_memory_page_capacity(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept {
        return value.second_memory_[index].workspace.batch_capacity();
    }
    struct MemoryFacts final {
        std::uint64_t bytes{};
        std::uint64_t page{};
        std::uint64_t total_pages{};
        std::size_t batch_capacity{};
        std::size_t node_capacity{};
    };
    [[nodiscard]] static auto first_memory_facts(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept {
        return memory_facts(value.first_memory_[index]);
    }
    [[nodiscard]] static auto second_memory_facts(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept {
        return memory_facts(value.second_memory_[index]);
    }
    [[nodiscard]] static auto initializer_evidence(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const kernel::RegionContentEvidence& { return value.initializer_.evidence(); }
    [[nodiscard]] static auto monitoring_identity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return PairwiseMonitoringPreparationTestAccess::storage_identity(
            value.monitoring_);
    }
    [[nodiscard]] static auto first_memory_request(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept
        -> const memory::RegionRequest& { return value.first_memory_[index].request; }
    [[nodiscard]] static auto second_memory_request(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept
        -> const memory::RegionRequest& { return value.second_memory_[index].request; }
    [[nodiscard]] static auto first_memory_storage(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept {
        return std::array<const void*, 2U>{&value.first_memory_[index].request,
                                           &value.first_memory_[index].workspace};
    }
    [[nodiscard]] static auto second_memory_storage(
        const PairwiseTypedPreOwnerPreparation& value, std::size_t index) noexcept {
        return std::array<const void*, 2U>{&value.second_memory_[index].request,
                                           &value.second_memory_[index].workspace};
    }
    [[nodiscard]] static auto first_request(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.first_request_;
    }
    [[nodiscard]] static auto second_request(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.second_request_;
    }
    [[nodiscard]] static auto first_identity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const AdmittedPlacementIdentity& { return value.first_identity_; }
    [[nodiscard]] static auto second_identity(
        const PairwiseTypedPreOwnerPreparation& value) noexcept
        -> const AdmittedPlacementIdentity& { return value.second_identity_; }
    [[nodiscard]] static auto implementation_id(
        const PairwiseTypedPreOwnerPreparation& value) noexcept {
        return value.implementation_id_;
    }
    [[nodiscard]] static auto observation_destination_count(
        const PairwiseTypedPreOwnerPreparation& value) noexcept -> std::size_t {
        return std::visit([](const auto& destinations) {
            return destinations.size();
        }, value.observation_workers_);
    }
    [[nodiscard]] static auto observation_worker_capacity(
        const PairwiseTypedPreOwnerPreparation& value,
        std::size_t index) noexcept -> std::size_t {
        return std::visit([index](const auto& destinations) {
            return destinations[index].capacity();
        }, value.observation_workers_);
    }
    [[nodiscard]] static auto observation_worker_storage(
        const PairwiseTypedPreOwnerPreparation& value,
        std::size_t index) noexcept -> const void* {
        return std::visit([index](const auto& destinations) -> const void* {
            return destinations[index].data();
        }, value.observation_workers_);
    }
  private:
    [[nodiscard]] static auto memory_facts(
        const PairwisePreparedMemoryInput& value) noexcept -> MemoryFacts {
        return {value.request.requested_bytes().value(),
                value.request.page_size().value(), value.workspace.total_pages(),
                value.workspace.batch_capacity(), value.workspace.node_capacity()};
    }
};

[[nodiscard]] auto prepare_pairwise_typed_pre_owner(
    const AdmittedPairwiseProductionInputs&,
    const PairwiseTypedPreOwnerDependencies&, std::optional<PreparationGraphLimits> = std::nullopt) noexcept
    -> PairwiseOperationResult<PairwiseTypedPreOwnerPreparation>;

// Consumes the allocation-only prefix and performs the real, mechanically
// unselected owner transaction. The returned generic envelope retains the
// concrete context for the subsequent binding slice.
[[nodiscard]] auto prepare_pairwise_typed_owner_transaction_storage(
    PairwiseTypedPreOwnerPreparation&&, PairwiseProgressState* = nullptr) noexcept
    -> PairwiseOperationResult<PairwiseTypedPreparedOwners>;
[[nodiscard]] auto prepare_pairwise_typed_owner_transaction(
    PairwiseTypedPreOwnerPreparation&&) noexcept
    -> PairwiseOperationResult<PairwiseTypedPreparedOwners>;

// Narrow binding seam implemented beside the concrete erased context.  The
// exact seven-function table never escapes the owner transaction TU.
[[nodiscard]] auto bind_pairwise_typed_owner_run(
    PairwiseTypedPreparedOwners&&) noexcept
    -> PairwiseOperationResult<PairwiseProductionRunCore>;

// Produces the existing operations envelope without selecting it in any
// public/runtime composition.  Its context is move-only and prepare_run is
// one-shot, including concurrent attempts.
[[nodiscard]] auto make_pairwise_production_operations(
    PairwiseTypedProductionBindingDependencies) noexcept
    -> PairwiseOperationResult<PairwiseProductionOperations>;
[[nodiscard]] auto make_pairwise_runner_execution_backend(
    PairwiseTypedProductionBindingDependencies) noexcept
    -> PairwiseOperationResult<PairwiseRunnerExecutionBackend>;

struct PairwiseTypedOwnerTransactionTestAccess final {
    struct State final {
        bool transferred{};
        bool team{};
        bool monitoring{};
        std::size_t first_owners{};
        std::size_t second_owners{};
        const void* first_owner_storage{};
        const void* second_owner_storage{};
        std::uint64_t measurement_root_seed{};
        std::uint64_t calibration_experiment_id{};
        std::uint64_t calibration_block_id{};
        std::array<const void*, 2U> latency_verification_storage{};
        std::array<std::size_t, 2U> latency_verification_capacities{};
        const void* operation_secondary_storage{};
        std::size_t operation_secondary_capacity{};
        std::array<const void*, 5U> finalization_storage{};
        std::array<std::size_t, 5U> finalization_capacity{};
        std::optional<kernel::SequentialReadBandwidthConfig> bandwidth_configuration;
        std::optional<kernel::DependentLoadLatencyConfig> latency_configuration;
        kernel::KernelImplementationId implementation_id{};
        std::size_t observation_destination_count{};
        std::size_t observation_worker_capacity{};
        bool bound_to_current_envelope{};
    };
    struct CommandFacts final {
        bool prepared{};
        bool started{};
        bool ended{};
        std::uint64_t work_units{};
        std::uint64_t logical_bytes{};
        std::uint64_t expected_terminal{};
        std::uint64_t actual_terminal{};
        bool verification_matched{};
    };
    [[nodiscard]] static auto state(
        const PairwiseTypedPreparedOwners&) noexcept -> State;
    [[nodiscard]] static auto command_facts(
        const PairwiseTypedPreparedOwners&, bool first,
        std::size_t worker) noexcept -> CommandFacts;
    [[nodiscard]] static auto observation_worker_storage(
        const PairwiseTypedPreparedOwners&, std::size_t) noexcept -> const void*;
    [[nodiscard]] static auto observation_worker_capacity(
        const PairwiseTypedPreparedOwners&, std::size_t) noexcept -> std::size_t;
    [[nodiscard]] static auto admit_observation(
        const PairwiseTypedPreparedOwners&,
        const measurement::ScheduledObservationSlot&) noexcept -> bool;
    [[nodiscard]] static auto calibrate_bandwidth(PairwiseTypedPreparedOwners&) noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::SequentialReadBandwidthCalibrationOutcome>;
    [[nodiscard]] static auto calibrate_latency(PairwiseTypedPreparedOwners&) noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::DependentLoadLatencyCalibrationOutcome>;
    [[nodiscard]] static auto warmup_bandwidth(
        PairwiseTypedPreparedOwners&, kernel::SequentialReadBandwidthPassCount) noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::SequentialReadBandwidthWarmupOutcome>;
    [[nodiscard]] static auto warmup_latency(
        PairwiseTypedPreparedOwners&, kernel::DependentLoadLatencyStepCount) noexcept
        -> PairwiseTypedOperationSourceResult<
            measurement::DependentLoadLatencyWarmupOutcome>;
    [[nodiscard]] static auto observe_bandwidth(
        PairwiseTypedPreparedOwners&,
        const measurement::SequentialReadBandwidthObservationRequiredAction&) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence>;
    [[nodiscard]] static auto observe_latency(
        PairwiseTypedPreparedOwners&,
        const measurement::DependentLoadLatencyObservationRequiredAction&) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence>;
    [[nodiscard]] static auto observe_bandwidth_once(
        PairwiseTypedPreparedOwners&, kernel::SequentialReadBandwidthPassCount,
        measurement::TimerOverheadEstimate) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence>;
    [[nodiscard]] static auto observe_latency_once(
        PairwiseTypedPreparedOwners&, kernel::DependentLoadLatencyStepCount,
        measurement::TimerOverheadEstimate) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence>;
    [[nodiscard]] static auto observe_bandwidth_at(
        PairwiseTypedPreparedOwners&, kernel::SequentialReadBandwidthPassCount,
        measurement::TimerOverheadEstimate, std::size_t ordinal) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence>;
    [[nodiscard]] static auto observe_latency_at(
        PairwiseTypedPreparedOwners&, kernel::DependentLoadLatencyStepCount,
        measurement::TimerOverheadEstimate, std::size_t ordinal) noexcept
        -> PairwiseTypedOperationSourceResult<FactualObservationEvidence>;
    [[nodiscard]] static auto finalize(PairwiseTypedPreparedOwners&) noexcept
        -> PairwiseFinalizationSource;
};

} // namespace epycbench::runtime::detail
