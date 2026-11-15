#include "runtime/pairwise_typed_production_binding_preparation.hpp"
#include "epycbench/kernel/registry.hpp"

#include "platform_linux/bounded_sysfs_operations.hpp"
#include "platform_linux/numa_operations.hpp"
#include "runtime/pairwise_kernel_leaf_facade.hpp"
#include "runtime/pairwise_measurement_leaf_facade.hpp"

#include <limits>

namespace epycbench::runtime::detail {
namespace {

auto operation_failure(PairwiseEvidenceFailure primary,
                       std::vector<PairwiseEvidenceFailure> secondary = {}) noexcept
    -> PairwiseOperationFailure {
    return {std::move(primary), std::move(secondary), std::nullopt};
}

template <class Failure>
auto operation_failure(Failure failure) noexcept -> PairwiseOperationFailure {
    return operation_failure(PairwiseEvidenceFailure{std::move(failure)});
}

auto runtime_preparation_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseOperationFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value) std::terminate();
    return operation_failure(std::move(*value));
}

auto checked_size(std::uint64_t value) noexcept -> std::optional<std::size_t> {
    if constexpr (std::numeric_limits<std::size_t>::max() <
                  std::numeric_limits<std::uint64_t>::max())
        if (value > std::numeric_limits<std::size_t>::max())
            return std::nullopt;
    return static_cast<std::size_t>(value);
}

auto checked_count(std::size_t value, std::size_t multiplier,
                   std::size_t addition) noexcept -> std::optional<std::size_t> {
    if (value > (std::numeric_limits<std::size_t>::max() - addition) /
                    multiplier)
        return std::nullopt;
    return value * multiplier + addition;
}

auto comparison_strategy(scenario::MatrixComparisonOrder value) noexcept
    -> measurement::ComparisonOrderStrategy {
    switch (value) {
    case scenario::MatrixComparisonOrder::randomized_abba:
        return measurement::ComparisonOrderStrategy::randomized_abba;
    case scenario::MatrixComparisonOrder::randomized_baab:
        return measurement::ComparisonOrderStrategy::randomized_baab;
    case scenario::MatrixComparisonOrder::randomized_balanced:
        return measurement::ComparisonOrderStrategy::randomized_balanced;
    }
    std::terminate();
}

auto monitoring_failure(PairwiseMonitoringPreparationFailure failure,
                        std::vector<PairwiseEvidenceFailure> secondary)
    -> PairwiseOperationFailure {
    auto cursor = PairwiseMonitoringFailureCursor::consume(std::move(failure));
    if (cursor.empty()) std::terminate();
    auto primary = cursor.next();
    while (!cursor.empty()) secondary.push_back(cursor.next());
    return operation_failure(std::move(primary), std::move(secondary));
}

auto worker_specs(const AdmittedPlacementRequest& placement)
    -> std::vector<WorkerLaunchSpec> {
    std::vector<WorkerLaunchSpec> result;
    result.reserve(placement.workers().size());
    for (std::size_t index = 0; index < placement.workers().size(); ++index) {
        const auto& worker = placement.workers()[index];
        const auto& cpu = worker.request().cpu();
        result.push_back({index, core::WorkerId{static_cast<std::uint32_t>(index)},
                          worker.processing_unit(), worker.core(),
                          cpu.stable_pu_id(), cpu.stable_core_id(), cpu.linux_cpu()});
    }
    return result;
}

auto memory_input(const AdmittedPlacementRequest& placement, std::size_t index)
    -> memory::RegionRequestInput {
    const auto& admitted = placement.workers()[index];
    const auto& source = admitted.request().region();
    auto ratio = memory::ExactRatio::create(
        source.placement_threshold().numerator(),
        source.placement_threshold().denominator());
    if (!ratio) std::terminate(); // admitted Scenario invariant
    std::vector<memory::RegionNumaTarget> targets;
    targets.reserve(source.nodes().size());
    for (std::size_t node = 0; node < source.nodes().size(); ++node)
        targets.push_back({source.nodes()[node].stable_numa_node_id(),
                           source.nodes()[node].linux_numa_node(),
                           admitted.numa_nodes()[node]});
    return {.worker_id = core::WorkerId{static_cast<std::uint32_t>(index)},
            .requested_bytes = source.bytes(),
            .page_size = source.page_size_bytes(),
            .placement_policy = memory::PlacementPolicy::bind,
            .page_mode = memory::PageMode::base,
            .numa_targets = std::move(targets),
            .placement_threshold = *ratio,
            .prefault = source.prefault(),
            .verify_before = source.verify_before(),
            .verify_after = source.verify_after()};
}

auto prepare_memory_inputs(const AdmittedPlacementRequest& placement,
                           const PairwiseMemoryOwningFacade& memory)
    -> std::expected<std::vector<PairwisePreparedMemoryInput>,
                     PairwiseEvidenceFailure> {
    const auto& topology = placement.context().topology();
    const auto& machine = topology.machine();
    auto domain = platform_linux::detail::NumaAccess::checked_domain(
        machine.present_nodes);
    if (!domain)
        return std::unexpected{runtime_preparation_failure(
            PairwiseFailureReason::contained_exception).primary};
    if (machine.page_sizes.empty() || !machine.effective_nodes.value())
        return std::unexpected{runtime_preparation_failure(
            PairwiseFailureReason::contained_exception).primary};
    const auto base_page = std::ranges::min(machine.page_sizes).value();
    std::vector<PairwisePreparedMemoryInput> result;
    result.reserve(placement.workers().size());
    for (std::size_t index = 0; index < placement.workers().size(); ++index) {
        const auto& admitted = placement.workers()[index];
        bool effective = true;
        bool online = true;
        bool memory_backed = true;
        for (const auto& selected : admitted.request().region().nodes()) {
            const auto linux = selected.linux_numa_node().value();
            const auto legacy = core::NumaNodeId{linux};
            effective &= machine.effective_nodes.value()->contains(legacy);
            online &= machine.online_nodes.contains(legacy);
            const auto found = std::ranges::find_if(
                topology.numa_nodes(), [&](const auto& node) {
                    return node.linux_node_id.value() &&
                           *node.linux_node_id.value() == linux;
                });
            memory_backed &= found != topology.numa_nodes().end() &&
                found->installed_memory.value() &&
                found->installed_memory.value()->value() != 0U;
        }
        auto facts = platform_linux::detail::NumaBindInvocationFacts::create(
            base_page, effective, online, memory_backed);
        if (!facts)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception).primary};
        auto request = memory.request(memory_input(placement, index));
        if (!request)
            return std::unexpected{PairwiseEvidenceFailure{std::move(request.error())}};
        auto mask = memory.prepare_mask(*request, *domain, *facts);
        if (!mask)
            return std::unexpected{PairwiseEvidenceFailure{std::move(mask.error())}};
        const auto bytes = request->requested_bytes().value();
        const auto page = request->page_size().value();
        if (page == 0U || bytes % page != 0U)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception).primary};
        auto workspace = memory.prepare_workspace(std::move(*mask), bytes / page);
        if (!workspace)
            return std::unexpected{PairwiseEvidenceFailure{std::move(workspace.error())}};
        result.push_back({std::move(*request), std::move(*workspace)});
    }
    return result;
}

} // namespace

auto prepare_pairwise_worker_specs(const AdmittedPlacementRequest& placement)
    -> std::vector<WorkerLaunchSpec> {
    return worker_specs(placement);
}

auto prepare_pairwise_memory_inputs(const AdmittedPlacementRequest& placement,
                                   const PairwiseMemoryOwningFacade& memory)
    -> std::expected<std::vector<PairwisePreparedMemoryInput>, PairwiseEvidenceFailure> {
    return prepare_memory_inputs(placement, memory);
}

PairwiseTypedPreOwnerPreparation::PairwiseTypedPreOwnerPreparation(
    std::unique_ptr<RunnerCancellation> cancellation,
    std::vector<WorkerLaunchSpec> workers,
    measurement::CalibrationDurationPolicy calibration,
    measurement::WarmupPolicy warmup, measurement::RepetitionPolicy repetition,
    PairwisePreparedTypedMeasurement measurement, PairwiseTypedAdapterStorage adapter,
    PairwisePreparedWorkerTeam worker_team,
    PairwisePreparedMonitoringStorage monitoring,
    std::vector<PairwisePreparedMemoryInput> first_memory,
    std::vector<PairwisePreparedMemoryInput> second_memory,
    std::vector<PairwisePreparedMemoryOwnerDestination> first_memory_owners,
    std::vector<PairwisePreparedMemoryOwnerDestination> second_memory_owners,
    PairwisePlatformLeafFacade platform, PairwiseKernelLeafFacade kernel,
    PairwiseWorkerOwningDependencies worker_dependencies,
    PairwisePreparedRegionInitializer initializer,
    ipc::RequestId first_request, ipc::RequestId second_request,
    AdmittedPlacementIdentity first_identity,
    AdmittedPlacementIdentity second_identity,
    PairwiseTypedPreparedFamily family,
    std::uint64_t comparison_order_seed,
    monitoring::MonitoringExecutionPolicy monitoring_policy,
    ServicePlacementPlan service_placement,
    StatisticsProductionPolicy statistics_policy,
    measurement::ComparisonOrderPolicy comparison_order,
    measurement::ObservationValidityPolicy validity,
    bool calibration_enabled,
    std::size_t maximum_blocks,
    std::optional<kernel::SequentialReadBandwidthConfig> bandwidth_configuration,
    std::optional<kernel::DependentLoadLatencyConfig> latency_configuration,
    std::optional<kernel::PreparedDependentLoadLatencyVerification> latency_verification,
    kernel::KernelImplementationId implementation_id,
    PairwisePreparedObservationWorkers observation_workers,
    std::uint64_t resolved_latency_seed,
    std::uint64_t measurement_root_seed,
    core::ExperimentId calibration_experiment_id,
    kernel::DependentLoadLatencyBlockId calibration_block_id,
    std::vector<PairwiseEvidenceFailure> operation_secondary,
    std::vector<PairwisePreparedFinalizationSlot> first_finalization_slots,
    std::vector<PairwisePreparedFinalizationSlot> second_finalization_slots,
    std::vector<FinalizedWorkerEvidence> first_finalized_workers,
    std::vector<FinalizedWorkerEvidence> second_finalized_workers,
    std::vector<LocalFinalizationErrorAssociation> finalization_associations) noexcept
    : cancellation_{std::move(cancellation)}, worker_specs_{std::move(workers)},
      calibration_{calibration}, warmup_{warmup}, repetition_{repetition},
      comparison_order_{comparison_order}, validity_{validity},
      calibration_enabled_{calibration_enabled},
      measurement_{std::move(measurement)}, adapter_{std::move(adapter)},
      worker_team_{std::move(worker_team)}, monitoring_{std::move(monitoring)},
      first_memory_{std::move(first_memory)}, second_memory_{std::move(second_memory)},
      first_memory_owners_{std::move(first_memory_owners)},
      second_memory_owners_{std::move(second_memory_owners)},
      initializer_{std::move(initializer)},
      platform_{std::move(platform)}, kernel_{std::move(kernel)},
      worker_dependencies_{std::move(worker_dependencies)},
      first_request_{first_request}, second_request_{second_request},
      first_identity_{std::move(first_identity)},
      second_identity_{std::move(second_identity)}, family_{family},
      comparison_order_seed_{comparison_order_seed},
      monitoring_policy_{std::move(monitoring_policy)},
      service_placement_{std::move(service_placement)},
      statistics_policy_{std::move(statistics_policy)},
      maximum_blocks_{maximum_blocks},
      bandwidth_configuration_{std::move(bandwidth_configuration)},
      latency_configuration_{std::move(latency_configuration)},
      latency_verification_{std::move(latency_verification)},
      implementation_id_{implementation_id},
      observation_workers_{std::move(observation_workers)},
      resolved_latency_seed_{resolved_latency_seed},
      measurement_root_seed_{measurement_root_seed},
      calibration_experiment_id_{calibration_experiment_id},
      calibration_block_id_{calibration_block_id},
      operation_secondary_{std::move(operation_secondary)},
      first_finalization_slots_{std::move(first_finalization_slots)},
      second_finalization_slots_{std::move(second_finalization_slots)},
      first_finalized_workers_{std::move(first_finalized_workers)},
      second_finalized_workers_{std::move(second_finalized_workers)},
      finalization_associations_{std::move(finalization_associations)} {}

PairwiseTypedPreOwnerPreparation::PairwiseTypedPreOwnerPreparation(
    PairwiseTypedPreOwnerPreparation&& other) noexcept
    : cancellation_{std::move(other.cancellation_)},
      shared_cancellation_{std::move(other.shared_cancellation_)},
      worker_specs_{std::move(other.worker_specs_)},
      calibration_{other.calibration_}, warmup_{other.warmup_},
      repetition_{other.repetition_}, comparison_order_{other.comparison_order_},
      validity_{other.validity_}, calibration_enabled_{other.calibration_enabled_},
      measurement_{std::move(other.measurement_)},
      adapter_{std::move(other.adapter_)}, worker_team_{std::move(other.worker_team_)},
      monitoring_{std::move(other.monitoring_)},
      first_memory_{std::move(other.first_memory_)},
      second_memory_{std::move(other.second_memory_)},
      first_memory_owners_{std::move(other.first_memory_owners_)},
      second_memory_owners_{std::move(other.second_memory_owners_)},
      initializer_{std::move(other.initializer_)},
      platform_{std::move(other.platform_)}, kernel_{std::move(other.kernel_)},
      worker_dependencies_{std::move(other.worker_dependencies_)},
      first_request_{other.first_request_}, second_request_{other.second_request_},
      first_identity_{std::move(other.first_identity_)},
      second_identity_{std::move(other.second_identity_)}, family_{other.family_},
      comparison_order_seed_{other.comparison_order_seed_},
      monitoring_policy_{std::move(other.monitoring_policy_)},
      service_placement_{std::move(other.service_placement_)},
      statistics_policy_{std::move(other.statistics_policy_)},
      maximum_blocks_{other.maximum_blocks_},
      bandwidth_configuration_{std::move(other.bandwidth_configuration_)},
      latency_configuration_{std::move(other.latency_configuration_)},
      latency_verification_{std::move(other.latency_verification_)},
      implementation_id_{other.implementation_id_},
      observation_workers_{std::move(other.observation_workers_)},
      resolved_latency_seed_{other.resolved_latency_seed_},
      measurement_root_seed_{other.measurement_root_seed_},
      calibration_experiment_id_{other.calibration_experiment_id_},
      calibration_block_id_{other.calibration_block_id_},
      operation_secondary_{std::move(other.operation_secondary_)},
      first_finalization_slots_{std::move(other.first_finalization_slots_)},
      second_finalization_slots_{std::move(other.second_finalization_slots_)},
      first_finalized_workers_{std::move(other.first_finalized_workers_)},
      second_finalized_workers_{std::move(other.second_finalized_workers_)},
      finalization_associations_{std::move(other.finalization_associations_)},
      active_{std::exchange(other.active_, false)}, preparation_graph_{std::move(other.preparation_graph_)} {}

auto prepare_pairwise_typed_pre_owner(
    const AdmittedPairwiseProductionInputs& inputs,
    const PairwiseTypedPreOwnerDependencies& dependencies, std::optional<PreparationGraphLimits> graph_limits) noexcept
    -> PairwiseOperationResult<PairwiseTypedPreOwnerPreparation> {
    try {
        const auto& first = inputs.pairwise_request().first();
        const auto& second = inputs.pairwise_request().second();
        const bool production_policy = dependencies.policy == stage_b_policy::Selection::stage_b_v1;
        if (production_policy && (!stage_b_policy::admit(inputs) ||
            (graph_limits && !stage_b_policy::admit(*graph_limits))))
            return std::unexpected{runtime_preparation_failure(PairwiseFailureReason::contained_exception)};
        std::vector<PairwiseEvidenceFailure> capacity_secondary;
        capacity_secondary.reserve(1U);
        if (dependencies.capacity_producer == nullptr)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        auto accepted = dependencies.capacity_producer->prepare(inputs);
        if (!accepted) {
            if (accepted.error().cleanup)
                capacity_secondary.push_back(std::move(*accepted.error().cleanup));
            return std::unexpected{operation_failure(
                std::move(accepted.error().primary), std::move(capacity_secondary))};
        }
        auto handoff = PairwiseTypedLocalMonitoringCapacityProducer::release(
            std::move(*accepted));
        if (!handoff)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        auto binding = PairwiseTypedMonitoringCapacityBinding::create(
            std::move(*handoff), inputs);
        if (!binding)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        PairwiseTypedMonitoringPrepareContext capacity_context{std::move(*binding)};
        auto capacity = capacity_context.prepare(inputs);
        if (!capacity) {
            if (capacity.error().runtime_failure())
                return std::unexpected{operation_failure(
                    std::move(capacity.error()).release_runtime_failure())};
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        }
        if (production_policy &&
            (capacity->admission().authority().present_cpus().size() > stage_b_policy::monitoring_domains ||
             capacity->admission().authority().kernel_maximum_cpu() >= stage_b_policy::node_bits ||
             capacity->enumeration_capacity() > stage_b_policy::monitoring_domains ||
             capacity->maximum_domain_count() > stage_b_policy::monitoring_domains ||
             capacity->maximum_membership_count() > stage_b_policy::monitoring_members ||
             capacity->preparation_rollback_values() > stage_b_policy::count ||
             capacity->admission().authority().base_page_bytes().value() > stage_b_policy::monitoring_text))
            return std::unexpected{runtime_preparation_failure(PairwiseFailureReason::contained_exception)};
        const auto monitoring_failure_capacity = checked_size(
            capacity->preparation_rollback_values());
        if (!monitoring_failure_capacity)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        std::vector<PairwiseEvidenceFailure> monitoring_secondary;
        monitoring_secondary.reserve(*monitoring_failure_capacity);
        if (first.workers().size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        if (first.workers().size() != second.workers().size())
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        const auto first_specs = worker_specs(first);
        const auto second_specs = worker_specs(second);
        if (first_specs.size() != second_specs.size() ||
            !std::ranges::equal(first_specs, second_specs, [](const auto& left,
                                                              const auto& right) {
                return left.worker_index == right.worker_index &&
                    left.worker_id == right.worker_id &&
                    left.processing_unit == right.processing_unit &&
                    left.core == right.core && left.stable_pu == right.stable_pu &&
                    left.stable_core == right.stable_core &&
                    left.requested_cpu == right.requested_cpu;
            }) || first.execution_inputs().kernel() !=
                    second.execution_inputs().kernel())
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        const auto& platform = dependencies.platform;
        const auto& authority = capacity->admission().authority();
        const auto present_count = authority.present_cpus().size();
        if (present_count > std::numeric_limits<std::uint64_t>::max() - 1U)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        const auto page = authority.base_page_bytes().value();
        const auto enumeration = capacity->enumeration_capacity();
        const auto memberships = capacity->maximum_membership_count();
        const auto maximum_cpu = authority.kernel_maximum_cpu();
        const auto maximum_cpu64 = static_cast<std::uint64_t>(maximum_cpu);
        std::vector<core::LinuxCpuId> present_cpus{
            authority.present_cpus().begin(), authority.present_cpus().end()};
        std::vector<core::LinuxCpuId> admitted_worker_cpus{
            capacity->admission().worker_cpus().begin(),
            capacity->admission().worker_cpus().end()};
        const auto graph_domains = checked_size(capacity->maximum_domain_count());
        const auto graph_memberships = checked_size(memberships);
        const auto graph_labels = checked_size(page);
        if (graph_limits && (!graph_domains || !graph_memberships || !graph_labels))
            return std::unexpected{runtime_preparation_failure(PairwiseFailureReason::contained_exception)};
        const PreparationGraphDimensions graph_dimensions{
            graph_domains.value_or(0U), graph_memberships.value_or(0U), graph_labels.value_or(0U)};
        if (production_policy && capacity->temperature_requested() &&
            !stage_b_policy::admit_hwmon({static_cast<std::uint64_t>(present_count), enumeration, page}))
            return std::unexpected{runtime_preparation_failure(PairwiseFailureReason::contained_exception)};
        auto monitoring = PairwiseMonitoringOwningFacade::prepare_capacity_storage(
            std::move(*capacity), platform,
            {.temperature_capacity = {static_cast<std::uint64_t>(present_count),
                                      enumeration, page},
             .temperature_operations = dependencies.temperature_operations,
             .frequency_capacity = {maximum_cpu64,
                                    maximum_cpu64 + 2U, maximum_cpu64 + 1U,
                                    memberships, page},
             .present_cpus = present_cpus,
             .worker_cpus = admitted_worker_cpus,
             .frequency_operations = dependencies.frequency_operations});
        if (!monitoring)
            return std::unexpected{monitoring_failure(
                std::move(monitoring.error()), std::move(monitoring_secondary))};

        const auto& source = first.execution_inputs().measurement();
        const PairwiseMeasurementLeafFacade measurement;
        auto calibration = measurement.calibration_duration_policy(
            source.observation_minimum_duration(),
            source.observation_target_duration(),
            source.observation_maximum_duration());
        if (!calibration)
            return std::unexpected{operation_failure(std::move(calibration.error()))};
        auto threshold = measurement.warmup_stability_threshold(
            source.warmup_stability_threshold().numerator(),
            source.warmup_stability_threshold().denominator());
        if (!threshold)
            return std::unexpected{operation_failure(std::move(threshold.error()))};
        auto warmup = measurement.warmup_policy(
            source.warmup_minimum(), source.warmup_maximum(),
            source.warmup_stability_window(), *threshold);
        if (!warmup)
            return std::unexpected{operation_failure(std::move(warmup.error()))};
        auto repetition = measurement.repetition_policy(
            source.repetition_minimum_blocks(), source.repetition_maximum_blocks(),
            source.minimum_valid_observations_per_variant(),
            source.stopping_enabled());
        if (!repetition)
            return std::unexpected{operation_failure(std::move(repetition.error()))};
        const auto comparison_order = measurement::ComparisonOrderPolicy::create(
            comparison_strategy(source.comparison_order()),
            inputs.pairwise_request().resolved_comparison_order_seed());
        const auto& overhead_source = source.maximum_harness_overhead();
        auto overhead = measurement.maximum_harness_overhead(
            overhead_source.numerator(), overhead_source.denominator());
        if (!overhead)
            return std::unexpected{operation_failure(std::move(overhead.error()))};
        const auto validity = measurement::ObservationValidityPolicy::create(*overhead);
        const auto blocks = checked_size(source.repetition_maximum_blocks());
        if (!blocks)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};

        auto adapter = PairwiseTypedAdapterStorage::create(
            first.workers().size(), *blocks);
        if (!adapter)
            return std::unexpected{operation_failure(std::move(adapter.error()))};

        auto prepared_measurement = [&]()
            -> PairwiseOperationResult<PairwisePreparedTypedMeasurement> {
            if (std::holds_alternative<ipc::PlacementBandwidthExecutionInputs>(
                    first.execution_inputs().kernel())) {
                auto value = measurement.prepare_bandwidth_execution(*calibration, *warmup);
                if (!value)
                    return std::unexpected{operation_failure(std::move(value.error()))};
                return PairwisePreparedTypedMeasurement{std::move(*value)};
            }
            auto value = measurement.prepare_latency_execution(*calibration, *warmup);
            if (!value)
                return std::unexpected{operation_failure(std::move(value.error()))};
            return PairwisePreparedTypedMeasurement{std::move(*value)};
        }();
        if (!prepared_measurement)
            return std::unexpected{std::move(prepared_measurement.error())};

        auto cancellation = dependencies.shared_cancellation
            ? std::unique_ptr<RunnerCancellation>{}
            : std::make_unique<RunnerCancellation>();
        auto& selected_cancellation = dependencies.shared_cancellation
            ? *dependencies.shared_cancellation : *cancellation;
        auto specs = first_specs;
        auto prepared_team = PairwiseWorkerOwningFacade::prepare(
            specs, selected_cancellation, *warmup, *repetition, platform);
        if (!prepared_team)
            return std::unexpected{operation_failure(std::move(prepared_team.error()))};

        const auto& kernel = dependencies.kernel;
        const PairwiseMemoryOwningFacade memory{platform, kernel};
        auto first_memory = prepare_memory_inputs(first, memory);
        if (!first_memory)
            return std::unexpected{operation_failure(std::move(first_memory.error()))};
        auto second_memory = prepare_memory_inputs(second, memory);
        if (!second_memory)
            return std::unexpected{operation_failure(std::move(second_memory.error()))};
        const auto worker_count = first.workers().size();
        const auto finalization_error_capacity = checked_count(worker_count, 6U, 6U);
        const auto finalization_association_capacity = checked_count(worker_count, 2U, 0U);
        if (!finalization_error_capacity || !finalization_association_capacity ||
            worker_count > std::vector<PairwisePreparedMemoryOwnerDestination>{}.max_size() ||
            worker_count > std::vector<PairwisePreparedFinalizationSlot>{}.max_size() ||
            worker_count > std::vector<FinalizedWorkerEvidence>{}.max_size() ||
            *finalization_association_capacity >
                std::vector<LocalFinalizationErrorAssociation>{}.max_size() ||
            *finalization_error_capacity >
                std::vector<PairwiseEvidenceFailure>{}.max_size())
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        std::vector<PairwisePreparedMemoryOwnerDestination> first_memory_owners(
            first_memory->size());
        std::vector<PairwisePreparedMemoryOwnerDestination> second_memory_owners(
            second_memory->size());
        auto initializer = [&]()
            -> std::expected<PairwisePreparedRegionInitializer, PairwiseKernelFailure> {
            const auto& family = first.execution_inputs().kernel();
            if (const auto* bandwidth = std::get_if<
                    ipc::PlacementBandwidthExecutionInputs>(&family))
                return kernel.prepare_region_initializer(
                    kernel::RegionInitializationSpec::bandwidth(
                        bandwidth->configuration()));
            const auto& latency =
                std::get<ipc::PlacementLatencyExecutionInputs>(family);
            auto specification = kernel.latency_region_spec(
                latency.configuration(), latency.resolved_permutation_seed());
            if (!specification)
                return std::unexpected{std::move(specification.error())};
            return kernel.prepare_region_initializer(*specification);
        }();
        if (!initializer)
            return std::unexpected{operation_failure(std::move(initializer.error()))};

        const auto family = std::holds_alternative<
            ipc::PlacementBandwidthExecutionInputs>(first.execution_inputs().kernel())
                ? PairwiseTypedPreparedFamily::bandwidth
                : PairwiseTypedPreparedFamily::latency;
        const auto* first_descriptor = epycbench::kernel::find_kernel_descriptor(
            first.request().experiment().kernel_id());
        const auto* second_descriptor = epycbench::kernel::find_kernel_descriptor(
            second.request().experiment().kernel_id());
        if (first_descriptor == nullptr || second_descriptor == nullptr ||
            first_descriptor->kernel_id() != second_descriptor->kernel_id() ||
            first_descriptor->implementation_id() !=
                second_descriptor->implementation_id())
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        std::optional<kernel::SequentialReadBandwidthConfig> bandwidth_configuration;
        std::optional<kernel::DependentLoadLatencyConfig> latency_configuration;
        std::optional<kernel::PreparedDependentLoadLatencyVerification>
            latency_verification;
        std::uint64_t resolved_latency_seed{};
        if (const auto* bandwidth = std::get_if<
                ipc::PlacementBandwidthExecutionInputs>(
                &first.execution_inputs().kernel())) {
            bandwidth_configuration.emplace(bandwidth->configuration());
        } else {
            const auto& latency = std::get<ipc::PlacementLatencyExecutionInputs>(
                first.execution_inputs().kernel());
            latency_configuration.emplace(latency.configuration());
            resolved_latency_seed = latency.resolved_permutation_seed();
            auto verification = kernel.latency_verification_for_configuration(
                latency.configuration(), resolved_latency_seed);
            if (!verification)
                return std::unexpected{operation_failure(
                    PairwiseEvidenceFailure{std::move(verification.error())})};
            latency_verification.emplace(std::move(*verification));
        }
        std::vector<PairwiseEvidenceFailure> operation_secondary;
        operation_secondary.reserve(*finalization_error_capacity);
        std::vector<FinalizedWorkerEvidence> first_finalized_workers;
        std::vector<FinalizedWorkerEvidence> second_finalized_workers;
        std::vector<LocalFinalizationErrorAssociation> finalization_associations;
        std::vector<PairwisePreparedFinalizationSlot> first_finalization_slots(
            first.workers().size());
        std::vector<PairwisePreparedFinalizationSlot> second_finalization_slots(
            first.workers().size());
        first_finalized_workers.reserve(first.workers().size());
        second_finalized_workers.reserve(first.workers().size());
        finalization_associations.reserve(*finalization_association_capacity);
        if (*blocks > std::numeric_limits<std::size_t>::max() / 4U)
            return std::unexpected{runtime_preparation_failure(
                PairwiseFailureReason::contained_exception)};
        const auto observation_count = *blocks * 4U;
        PairwisePreparedObservationWorkers observation_workers = [&]()
            -> PairwisePreparedObservationWorkers {
            if (family == PairwiseTypedPreparedFamily::bandwidth) {
                std::vector<std::vector<
                    measurement::SequentialReadBandwidthWorkerObservation>> values(
                        observation_count);
                for (auto& value : values)
                    value.reserve(first.workers().size());
                return values;
            }
            std::vector<std::vector<
                measurement::DependentLoadLatencyWorkerObservation>> values(
                    observation_count);
            for (auto& value : values)
                value.reserve(first.workers().size());
            return values;
        }();
        auto worker_dependencies = dependencies.worker
            ? *dependencies.worker
            : PairwiseWorkerOwningFacade::production_dependencies();
        if (!dependencies.worker)
            worker_dependencies.platform = dependencies.platform;
        auto result = PairwiseTypedPreOwnerPreparation{
            std::move(cancellation), std::move(specs), *calibration, *warmup,
            *repetition, std::move(*prepared_measurement), std::move(*adapter),
            std::move(*prepared_team), std::move(*monitoring),
            std::move(*first_memory), std::move(*second_memory),
            std::move(first_memory_owners), std::move(second_memory_owners),
            dependencies.platform, dependencies.kernel,
            std::move(worker_dependencies),
            std::move(*initializer), first.request_id(), second.request_id(),
            first.placement_identity(), second.placement_identity(), family,
            inputs.pairwise_request().resolved_comparison_order_seed(),
            inputs.monitoring_policy(), inputs.service_placement(),
            inputs.statistics_policy(), comparison_order, validity,
            source.calibration_enabled(), *blocks,
            std::move(bandwidth_configuration), std::move(latency_configuration),
            std::move(latency_verification), first_descriptor->implementation_id(),
            std::move(observation_workers), resolved_latency_seed,
            source.root_seed(),
            core::ExperimentId{first.request().experiment().index()},
            kernel::DependentLoadLatencyBlockId{0U},
            std::move(operation_secondary), std::move(first_finalization_slots),
            std::move(second_finalization_slots), std::move(first_finalized_workers),
            std::move(second_finalized_workers),
            std::move(finalization_associations)};
        result.shared_cancellation_ = dependencies.shared_cancellation;
        if (graph_limits) {
            auto graph=PairwisePreparationGraph::prepare(result,inputs,graph_dimensions,*graph_limits,
                MonitoringPreparationFactDomain::source);
            if (!graph) return std::unexpected{runtime_preparation_failure(PairwiseFailureReason::contained_exception)};
            result.preparation_graph_.emplace(std::move(*graph));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected{runtime_preparation_failure(
            PairwiseFailureReason::contained_exception)};
    } catch (...) {
        return std::unexpected{runtime_preparation_failure(
            PairwiseFailureReason::contained_unknown_failure)};
    }
}

} // namespace epycbench::runtime::detail
