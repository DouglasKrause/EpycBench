#include "runtime/pairwise_v2_progress.hpp"
#include "epycbench/runtime/pairwise_runner_execution_backend.hpp"

#include "runtime/pairwise_measurement_leaf_facade.hpp"
#include "runtime/pairwise_prepared_runner_execution.hpp"
#include "runtime/pairwise_production_run_core.hpp"
#include "runtime/pairwise_statistics_leaf_facade.hpp"
#include "runtime/pairwise_runner_execution_backend_test_access.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace epycbench::runtime {
using detail::PairwiseMeasurementLeafFacade;
using detail::PairwiseStatisticsLeafFacade;
using detail::PairwiseStatisticsWorkspace;

namespace {

std::atomic<void (*)()> pre_storage_hook{};
std::atomic<void (*)()> post_run_hook{};
std::atomic<void (*)()> post_finalize_hook{};

auto outer_backend_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{
                "runtime.production_backend.invariant_failed"},
            .message = "Typed pairwise backend could not preserve its contract",
            .operation = "runtime.production_backend.execute",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto typed_fatal_outer_error(PairwiseRuntimeFailure failure) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{
                "runtime.production_backend.invariant_failed"},
            .message = "Typed pairwise backend could not commit truthful evidence",
            .operation = "runtime.production_backend.execute",
            .os_error = std::nullopt,
            .context = {{"classification", "typed_fatal_commit"},
                        {"failure_code", static_cast<std::uint16_t>(failure.code())},
                        {"failure_reason",
                         static_cast<std::uint16_t>(failure.reason())}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto backend_failure(PairwiseFailureReason reason,
                     PairwiseFailureContext context) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_backend_execute,
        reason, std::move(context));
    if (!value)
        std::terminate();
    return PairwiseEvidenceFailure{*value};
}

auto post_run_failure(bool standard) noexcept -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        standard
            ? PairwiseFailureCode::runtime_production_backend_post_run_exception
            : PairwiseFailureCode::
                  runtime_production_backend_post_run_non_standard_exception,
        PairwiseFailureOperation::runtime_production_backend_execute,
        standard ? PairwiseFailureReason::exception
                 : PairwiseFailureReason::non_standard_exception,
        NoPairwiseFailureContext{});
    if (!value)
        std::terminate();
    return PairwiseEvidenceFailure{*value};
}

auto preparation_failure(PairwiseFailureReason reason) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations, reason,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return PairwiseEvidenceFailure{*value};
}

auto runtime_failure(const PairwiseEvidenceFailure& failure) noexcept
    -> PairwiseRuntimeFailure {
    const auto* value = std::get_if<PairwiseRuntimeFailure>(&failure);
    if (value == nullptr)
        std::terminate();
    return *value;
}

auto operation_failure(PairwiseEvidenceFailure failure)
    -> PairwiseOperationFailure {
    return {std::move(failure), {}, std::nullopt};
}

template <class Failure>
auto operation_failure(Failure failure) -> PairwiseOperationFailure {
    return operation_failure(PairwiseEvidenceFailure{std::move(failure)});
}

auto counts(std::uint64_t expected, std::uint64_t observed) noexcept
    -> PairwiseFailureContext {
    return ExpectedObservedCountContext{
        PairwiseCount{expected}, PairwiseCount{observed}};
}

auto capacity_failure(std::size_t expected, std::size_t observed) noexcept
    -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::force_error_capacity_exhausted,
        counts(expected, observed));
    if (!value)
        std::terminate();
    return PairwiseEvidenceFailure{*value};
}

auto ordinals(std::uint64_t expected, std::uint64_t observed) noexcept
    -> PairwiseFailureContext {
    return ExpectedObservedOrdinalContext{
        PairwiseOrdinal{expected}, PairwiseOrdinal{observed}};
}

auto worker_context(measurement::ComparisonVariantSlot slot,
                    std::size_t worker) noexcept -> PairwiseFailureContext {
    return WorkerAssociationContext{
        slot == measurement::ComparisonVariantSlot::first
            ? PairwiseRole::first : PairwiseRole::second,
        PairwiseOrdinal{worker}};
}

auto make_policies(const ipc::PlacementMeasurementInputs& source,
                   std::uint64_t order_seed)
    -> std::expected<std::tuple<measurement::RepetitionPolicy,
                                measurement::WarmupPolicy,
                                measurement::ComparisonOrderPolicy>, core::Error> {
    auto repetition = measurement::RepetitionPolicy::create(
        source.repetition_minimum_blocks(),
        source.repetition_maximum_blocks(),
        source.minimum_valid_observations_per_variant(),
        source.stopping_enabled());
    auto threshold = measurement::WarmupStabilityThreshold::create(
        source.warmup_stability_threshold().numerator(),
        source.warmup_stability_threshold().denominator());
    if (!repetition || !threshold)
        return std::unexpected{outer_backend_error("admitted_policy")};
    auto warmup = measurement::WarmupPolicy::create(
        source.warmup_minimum(), source.warmup_maximum(),
        source.warmup_stability_window(), *threshold);
    if (!warmup)
        return std::unexpected{outer_backend_error("admitted_policy")};
    const auto strategy = [&] {
        switch (source.comparison_order()) {
        case scenario::MatrixComparisonOrder::randomized_abba:
            return measurement::ComparisonOrderStrategy::randomized_abba;
        case scenario::MatrixComparisonOrder::randomized_baab:
            return measurement::ComparisonOrderStrategy::randomized_baab;
        case scenario::MatrixComparisonOrder::randomized_balanced:
            return measurement::ComparisonOrderStrategy::randomized_balanced;
        }
        return measurement::ComparisonOrderStrategy::randomized_abba;
    }();
    return std::tuple{
        *repetition, *warmup,
        measurement::ComparisonOrderPolicy::create(strategy, order_seed)};
}

auto calibration_policy(const AdmittedPairwiseProductionInputs& inputs)
    -> std::expected<measurement::CalibrationDurationPolicy, core::Error> {
    const auto& source =
        inputs.pairwise_request().first().execution_inputs().measurement();
    auto result = measurement::CalibrationDurationPolicy::create(
        source.observation_minimum_duration(),
        source.observation_target_duration(),
        source.observation_maximum_duration());
    if (!result)
        return std::unexpected{
            outer_backend_error("admitted_calibration_policy")};
    return result;
}

auto provenance(const AdmittedPairwiseProductionInputs& inputs)
    -> std::expected<PairwiseRunProvenance, core::Error> {
    const auto& request = inputs.pairwise_request().first().request();
    auto scenario = request.scenario_provenance();
    if (!scenario)
        return std::unexpected{scenario.error()};
    return PairwiseRunProvenance{
        core::ExperimentId{request.experiment().index()},
        "epycbench.scenario/v0.1",
        scenario->get().scenario_digest().text(),
        scenario->get().resolved_scenario_plan_digest().text(),
        request.experiment().workload_id().to_string(),
        request.experiment().id().to_string() + ":first",
        request.experiment().id().to_string() + ":second"};
}

auto finalize_failure(PairwiseEvidenceFailure failure) noexcept
    -> FinalizedEvidenceError {
    return FinalizedEvidenceError::from_failure(std::move(failure));
}

auto bandwidth_effect(const PairwiseBlockAttemptEvidence& block,
                      const PairwiseStatisticsLeafFacade& statistics)
    -> std::expected<statistics::PairedLogEffect,
                     PairwiseEvidenceFailure> {
    std::array<std::optional<statistics::Bandwidth>, 2> first{};
    std::array<std::optional<statistics::Bandwidth>, 2> second{};
    std::size_t first_count{};
    std::size_t second_count{};
    for (const auto& factual : block.observations) {
        const auto* observation =
            std::get_if<measurement::SequentialReadBandwidthObservation>(
                &factual.observation);
        if (!observation)
            return std::unexpected{backend_failure(
                PairwiseFailureReason::mixed_observation_family,
                KernelIdentityContext{
                    PairwiseKernelFamily::bandwidth,
                    PairwiseKernelImplementation::scalar_u64_sequential_v1,
                    PairwiseKernelPhase::none})};
        auto metric = statistics.bandwidth(
            observation->aggregate_work(),
            observation->enclosing_interval().elapsed());
        if (!metric)
            return std::unexpected{PairwiseEvidenceFailure{metric.error()}};
        if (factual.slot.comparison_variant() ==
            measurement::ComparisonVariantSlot::first)
            first[first_count++] = *metric;
        else
            second[second_count++] = *metric;
    }
    if (first_count != 2U || second_count != 2U)
        return std::unexpected{backend_failure(
            PairwiseFailureReason::block_cardinality,
            counts(4U, first_count + second_count))};
    auto representatives = statistics.bandwidth_representatives(
        {*first[0], *first[1]}, {*second[0], *second[1]});
    if (!representatives)
        return std::unexpected{
            PairwiseEvidenceFailure{representatives.error()}};
    auto effect = statistics.bandwidth_log_effect(*representatives);
    if (!effect)
        return std::unexpected{PairwiseEvidenceFailure{effect.error()}};
    return *effect;
}

auto latency_effect(const PairwiseBlockAttemptEvidence& block,
                    const PairwiseStatisticsLeafFacade& statistics)
    -> std::expected<statistics::PairedLogEffect,
                     PairwiseEvidenceFailure> {
    std::array<std::optional<statistics::DependentLoadLatency>, 2> first{};
    std::array<std::optional<statistics::DependentLoadLatency>, 2> second{};
    std::size_t first_count{};
    std::size_t second_count{};
    for (const auto& factual : block.observations) {
        const auto* observation =
            std::get_if<measurement::DependentLoadLatencyObservation>(
                &factual.observation);
        if (!observation)
            return std::unexpected{backend_failure(
                PairwiseFailureReason::mixed_observation_family,
                KernelIdentityContext{
                    PairwiseKernelFamily::latency,
                    PairwiseKernelImplementation::scalar_u64_random_cycle_v1,
                    PairwiseKernelPhase::none})};
        auto metric = statistics.dependent_load_latency(
            observation->enclosing_interval().elapsed(),
            observation->aggregate_work());
        if (!metric)
            return std::unexpected{PairwiseEvidenceFailure{metric.error()}};
        if (factual.slot.comparison_variant() ==
            measurement::ComparisonVariantSlot::first)
            first[first_count++] = *metric;
        else
            second[second_count++] = *metric;
    }
    if (first_count != 2U || second_count != 2U)
        return std::unexpected{backend_failure(
            PairwiseFailureReason::block_cardinality,
            counts(4U, first_count + second_count))};
    auto representatives = statistics.dependent_load_latency_representatives(
        {*first[0], *first[1]}, {*second[0], *second[1]});
    if (!representatives)
        return std::unexpected{
            PairwiseEvidenceFailure{representatives.error()}};
    auto effect = statistics.dependent_load_latency_log_effect(*representatives);
    if (!effect)
        return std::unexpected{PairwiseEvidenceFailure{effect.error()}};
    return *effect;
}

struct PendingExecutionFacts final {
    std::optional<measurement::MeasurementBlockPlan> open_plan;
    std::optional<PairwiseCalibrationEvidence> calibration;
    std::optional<PairwiseWarmupEvidence> warmup;
    std::optional<FactualObservationEvidence> observation;
    std::optional<PairwiseOperationFailure> observation_failure;
    std::optional<PairwiseBlockAttemptEvidence> block;
    std::optional<RunnerStoppingEvaluationEvidence> stopping;
    std::optional<NormalRunTerminalEvidence> normal;
    std::optional<OperationalRunTerminalEvidence> terminal;
    std::optional<PairwiseFinalizationOutcome> finalization;
};

struct PreparedExecutionStorage final {
    std::vector<std::vector<FactualObservationEvidence>> observations;
    std::vector<std::vector<FinalizedEvidenceError>> failed_secondaries;
    std::vector<statistics::PairedLogEffect> effects;
    std::vector<FinalizedEvidenceError> terminal_secondaries;
    PairwiseStatisticsWorkspace statistics_workspace;
    PendingExecutionFacts pending{};
    detail::progress::Publication publication{};
};

auto prepare_execution_storage(std::size_t maximum_blocks,
                               std::size_t workers,
                               std::size_t bootstrap_replicates)
    -> PreparedExecutionStorage {
    if (workers > (std::numeric_limits<std::size_t>::max() - 8U) / 8U)
        throw std::bad_alloc{};
    const auto observation_secondaries =
        std::max(workers, std::size_t{2U}) + workers + 1U;
    const auto terminal_capacity = workers * 7U + 8U +
                                   std::max(workers, std::size_t{2U});
    std::vector<std::vector<FactualObservationEvidence>> observations(
        maximum_blocks);
    std::vector<std::vector<FinalizedEvidenceError>> failed_secondaries(
        maximum_blocks);
    for (std::size_t block = 0; block < maximum_blocks; ++block) {
        observations[block].reserve(4U);
        failed_secondaries[block].reserve(observation_secondaries);
    }
    std::vector<statistics::PairedLogEffect> effects;
    effects.reserve(maximum_blocks);
    std::vector<FinalizedEvidenceError> terminal_secondaries;
    terminal_secondaries.reserve(terminal_capacity);
    return {std::move(observations), std::move(failed_secondaries),
            std::move(effects), std::move(terminal_secondaries),
            PairwiseStatisticsWorkspace::prepare(maximum_blocks,
                                                 bootstrap_replicates)};
}

auto commit_without_run_fixed(PairwiseRunEvidenceLedger& ledger,
                        PairwiseOperationFailure& failure,
                        std::vector<FinalizedEvidenceError>& secondary,
                        PendingExecutionFacts& pending)
    -> std::expected<FinalizedPairwiseRunEvidence, PairwiseRuntimeFailure> {
    if (failure.secondary.size() >
        secondary.capacity() - secondary.size())
        return std::unexpected{runtime_failure(
            capacity_failure(secondary.capacity(),
                             secondary.size() + failure.secondary.size()))};
    for (auto& value : failure.secondary)
        secondary.push_back(finalize_failure(std::move(value)));
    pending.terminal.emplace(OperationalRunTerminalEvidence{
        finalize_failure(std::move(failure.primary)), std::move(secondary), failure.active_block_id});
    auto terminal = detail::PairwiseLedgerPreparedAccess::record_operational_terminal(ledger, *pending.terminal);
    if (!terminal)
        return std::unexpected{terminal.error()};
    auto finalized = std::move(ledger).finalize();
    if (!finalized)
        return std::unexpected{finalized.error()};
    return std::move(*finalized);
}

auto commit_without_run(PairwiseRunEvidenceLedger ledger,
                        PairwiseOperationFailure failure,
                        std::vector<FinalizedEvidenceError> secondary)
    -> std::expected<FinalizedPairwiseRunEvidence, core::Error> {
    PendingExecutionFacts pending;
    auto result = commit_without_run_fixed(ledger, failure, secondary, pending);
    if (!result) return std::unexpected{typed_fatal_outer_error(result.error())};
    return std::move(*result);
}

template <typename State>
auto stopping(PairwiseRunEvidenceLedger& ledger, State state,
              const measurement::PrecisionEvaluationRequiredAction& action,
              std::span<const statistics::PairedLogEffect> effects,
              const StatisticsProductionPolicy& policy,
              PairwiseStatisticsWorkspace& workspace,
              const PairwiseStatisticsLeafFacade& statistics,
              const PairwiseMeasurementLeafFacade& measurement,
              auto apply_decision, auto next_action, PendingExecutionFacts& pending,
    detail::progress::Publication& publication)
    -> std::expected<State, PairwiseOperationFailure> {
    auto comparison = statistics.aggregate(effects, workspace);
    if (!comparison)
        return std::unexpected{operation_failure(comparison.error())};
    auto bootstrap = statistics.bootstrap(
        effects, policy.stopping_bootstrap_policy(), workspace);
    if (!bootstrap)
        return std::unexpected{operation_failure(bootstrap.error())};
    auto evaluation = statistics.stopping(
        comparison->oriented_geometric_ratio(),
        bootstrap->oriented_geometric_ratio_interval(),
        policy.relative_half_width_target());
    if (!evaluation)
        return std::unexpected{operation_failure(evaluation.error())};
    const auto decision =
        evaluation->attainment() ==
                statistics::PrecisionTargetAttainment::target_met
            ? measurement::PrecisionStoppingDecision::target_met
            : measurement::PrecisionStoppingDecision::target_not_met;
    auto next = apply_decision(measurement, std::move(state), decision);
    if (!next)
        return std::unexpected{operation_failure(next.error())};
    auto disposition = RepetitionBoundaryDisposition::continued;
    const auto following = next_action(measurement, *next);
    if (const auto* complete =
            std::get_if<measurement::RepetitionCompleteAction>(&following)) {
        disposition = complete->outcome().disposition() ==
                              measurement::RepetitionDisposition::
                                  precision_target_reached
            ? RepetitionBoundaryDisposition::precision_target_reached
            : RepetitionBoundaryDisposition::maximum_blocks_reached;
    }
    const auto boundary = kernel::DependentLoadLatencyBlockId{
        action.completed_blocks() - 1U};
    pending.stopping.emplace(RunnerStoppingEvaluationEvidence{boundary, action.completed_blocks(), action.complete_valid_blocks(),
         next->valid_first_variant_observations(),
         next->valid_second_variant_observations(), *bootstrap, *evaluation,
         decision, disposition});
    if (!publication.emit(detail::progress::Kind::stopping, &*pending.stopping)) return std::move(*next);
    auto appended = detail::PairwiseLedgerPreparedAccess::append_stopping_evaluation(
        ledger, *pending.stopping);
    if (!appended)
        return std::unexpected{operation_failure(appended.error())};
    pending.stopping.reset();
    return std::move(*next);
}

template <typename State, typename Calibration, typename Warmup,
          typename ObservationAction, typename ObservationValue,
          typename Calibrate, typename RunWarmup, typename Observe,
          typename ApplyCalibration, typename ApplyWarmup,
          typename ApplyObservation, typename ApplyDecision, typename Next,
          typename MakeEffect>
auto execute_family(
    PairwiseRunEvidenceLedger& ledger, State state,
    PairwiseProductionRun& run,
    const StatisticsProductionPolicy& statistics_policy,
    const measurement::CalibrationDurationPolicy& calibration,
    const measurement::WarmupPolicy& warmup,
    PreparedExecutionStorage& storage,
    const PairwiseMeasurementLeafFacade& measurement,
    const PairwiseStatisticsLeafFacade& statistics,
    Calibrate calibrate, RunWarmup run_warmup, Observe observe,
    ApplyCalibration apply_calibration, ApplyWarmup apply_warmup,
    ApplyObservation apply_observation, ApplyDecision apply_decision,
    Next next, MakeEffect make_effect)
    -> PairwiseOperationResult<std::optional<measurement::RepetitionOutcome>> {
    auto& pending = storage.pending;
    std::optional<State> current{std::move(state)};
    auto& active_plan = pending.open_plan;
    std::size_t block_index{};
    for (;;) {
        auto action = next(measurement, *current);
        if (std::holds_alternative<Calibration>(action)) {
            auto outcome = calibrate(calibration);
            if (!outcome)
                return std::unexpected{std::move(outcome.error())};
            pending.calibration.emplace(std::move(*outcome));
            if (!storage.publication.emit(detail::progress::Kind::calibration, &*pending.calibration))
                return std::optional<measurement::RepetitionOutcome>{};
            auto associated = ledger.associate_calibration(std::move(*pending.calibration));
            if (!associated)
                return std::unexpected{operation_failure(associated.error())};
            pending.calibration.reset();
            auto applied = apply_calibration(
                measurement, std::move(*current),
                std::get<std::remove_cvref_t<decltype(*outcome)>>(
                    ledger.calibration_evidence()));
            if (!applied)
                return std::unexpected{operation_failure(applied.error())};
            current.emplace(std::move(*applied));
        } else if (const auto* warmup_action = std::get_if<Warmup>(&action)) {
            auto outcome = run_warmup(
                warmup, warmup_action->selected_state().selected_work());
            if (!outcome)
                return std::unexpected{std::move(outcome.error())};
            pending.warmup.emplace(std::move(*outcome));
            if (!storage.publication.emit(detail::progress::Kind::warmup, &*pending.warmup))
                return std::optional<measurement::RepetitionOutcome>{};
            auto associated = ledger.associate_warmup(std::move(*pending.warmup));
            if (!associated)
                return std::unexpected{operation_failure(associated.error())};
            pending.warmup.reset();
            auto applied = apply_warmup(
                measurement, std::move(*current),
                std::get<std::remove_cvref_t<decltype(*outcome)>>(
                    ledger.warmup_evidence()));
            if (!applied)
                return std::unexpected{operation_failure(applied.error())};
            current.emplace(std::move(*applied));
        } else if (const auto* observation_action =
                       std::get_if<ObservationAction>(&action)) {
            if (block_index >= storage.observations.size())
                return std::unexpected{operation_failure(backend_failure(
                    PairwiseFailureReason::block_cardinality,
                    counts(storage.observations.size(), block_index + 1U)))};
            if (!active_plan) {
                active_plan.emplace(observation_action->block_plan());
                if (!storage.publication.emit(detail::progress::Kind::block_opened, &*active_plan))
                    return std::optional<measurement::RepetitionOutcome>{};
            }
            auto factual = observe(run, *observation_action);
            if (!factual) {
                pending.observation_failure.emplace(std::move(factual.error()));
                auto& failure = *pending.observation_failure;
                auto& failed_secondary =
                    storage.failed_secondaries[block_index];
                if (failure.secondary.size() >
                    failed_secondary.capacity() - failed_secondary.size())
                    return std::unexpected{operation_failure(capacity_failure(
                        failed_secondary.capacity(),
                        failed_secondary.size() + failure.secondary.size()))};
                for (const auto& secondary : failure.secondary)
                    failed_secondary.push_back(finalize_failure(secondary));
                FailedObservationEvidence failed{
                    observation_action->slot(),
                    observation_action->observation_id(),
                    finalize_failure(failure.primary),
                    std::move(failed_secondary)};
                pending.block.emplace(PairwiseBlockAttemptEvidence{std::move(*active_plan),
                     std::move(storage.observations[block_index]),
                     std::move(failed),
                     BlockAttemptCompletion::operationally_incomplete});
                if (!storage.publication.emit(detail::progress::Kind::failed_observation,
                        &*pending.block->failed_observation) ||
                    !storage.publication.emit(detail::progress::Kind::block_closed, &*pending.block))
                    return std::optional<measurement::RepetitionOutcome>{};
                failure.active_block_id.emplace(observation_action->block_plan().block_id());
                if (storage.publication.pending_count) return std::unexpected{std::move(failure)};
                auto appended = detail::PairwiseLedgerPreparedAccess::append_block(ledger, *pending.block);
                if (!appended)
                    return std::unexpected{
                        operation_failure(appended.error())};
                pending.block.reset();
                failure.active_block_id.emplace(
                    observation_action->block_plan().block_id());
                return std::unexpected{std::move(failure)};
            }
            pending.observation.emplace(std::move(*factual));
            if (!storage.publication.emit(detail::progress::Kind::observation, &*pending.observation))
                return std::optional<measurement::RepetitionOutcome>{};
            auto applied = apply_observation(
                measurement, std::move(*current),
                std::get<ObservationValue>(pending.observation->observation),
                pending.observation->validity);
            if (!applied)
                return std::unexpected{operation_failure(applied.error())};
            auto& observations = storage.observations[block_index];
            if (observations.size() == observations.capacity())
                return std::unexpected{operation_failure(capacity_failure(
                    observations.capacity(), observations.size() + 1U))};
            observations.push_back(std::move(*pending.observation));
            pending.observation.reset();
            current.emplace(std::move(*applied));
            if (observations.size() == 4U) {
                const bool valid = std::ranges::all_of(
                    observations, [](const auto& value) {
                        return value.validity.disposition() ==
                               measurement::ObservationValidityDisposition::valid;
                    });
                pending.block.emplace(PairwiseBlockAttemptEvidence{
                    std::move(*active_plan), std::move(observations), std::nullopt,
                    valid ? BlockAttemptCompletion::complete_valid
                          : BlockAttemptCompletion::complete_invalid});
                auto& block = *pending.block;
                if (valid) {
                    auto effect = make_effect(block, statistics);
                    if (!effect)
                        return std::unexpected{
                            operation_failure(effect.error())};
                    if (storage.effects.size() == storage.effects.capacity())
                        return std::unexpected{operation_failure(
                            capacity_failure(storage.effects.capacity(),
                                             storage.effects.size() + 1U))};
                    storage.effects.push_back(*effect);
                }
                if (!storage.publication.emit(detail::progress::Kind::block_closed, &block))
                    return std::optional<measurement::RepetitionOutcome>{};
                auto appended = detail::PairwiseLedgerPreparedAccess::append_block(ledger, block);
                if (!appended)
                    return std::unexpected{
                        operation_failure(appended.error())};
                pending.block.reset();
                active_plan.reset();
                ++block_index;
            }
        } else if (const auto* precision_action =
                       std::get_if<
                           measurement::PrecisionEvaluationRequiredAction>(
                           &action)) {
            auto applied = stopping(
                ledger, std::move(*current), *precision_action,
                storage.effects, statistics_policy,
                storage.statistics_workspace, statistics, measurement,
                apply_decision, next, pending, storage.publication);
            if (!applied)
                return std::unexpected{std::move(applied.error())};
            current.emplace(std::move(*applied));
            if (storage.publication.interrupted) return std::optional<measurement::RepetitionOutcome>{};
        } else {
            return std::get<measurement::RepetitionCompleteAction>(action)
                .outcome();
        }
    }
}

template <class State>
auto execute_prepared_family(
    PairwiseRunEvidenceLedger& ledger, State state, PairwiseProductionRun& run,
    const StatisticsProductionPolicy& statistics_policy,
    const measurement::CalibrationDurationPolicy& calibration,
    const measurement::WarmupPolicy& warmup, PreparedExecutionStorage& storage)
    -> PairwiseOperationResult<std::optional<measurement::RepetitionOutcome>> {
    const PairwiseMeasurementLeafFacade measurement;
    const PairwiseStatisticsLeafFacade statistics;
    if constexpr (std::is_same_v<State, measurement::SequentialReadBandwidthRepetitionState>) {
        return execute_family<
                    measurement::SequentialReadBandwidthRepetitionState,
                    measurement::SequentialReadBandwidthCalibrationRequiredAction,
                    measurement::SequentialReadBandwidthWarmupRequiredAction,
                    measurement::SequentialReadBandwidthObservationRequiredAction,
                    measurement::SequentialReadBandwidthObservation>(
                    ledger, std::move(state), run,
                    statistics_policy, calibration, warmup, storage,
                    measurement, statistics,
                    [&](const auto& policy) {
                        return run.calibrate_bandwidth(policy);
                    },
                    [&](const auto& policy, auto work) {
                        return run.warmup_bandwidth(policy, work);
                    },
                    [](auto& session, const auto& action) {
                        return session.observe_bandwidth(action);
                    },
                    [](const auto& facade, auto value, const auto& outcome) {
                        return facade.apply_bandwidth_calibration(
                            std::move(value), outcome);
                    },
                    [](const auto& facade, auto value, const auto& outcome) {
                        return facade.apply_bandwidth_warmup(
                            std::move(value), outcome);
                    },
                    [](const auto& facade, auto value, const auto& observation,
                       const auto& validity) {
                        return facade.apply_bandwidth_observation(
                            std::move(value), observation, validity);
                    },
                    [](const auto& facade, auto value, auto decision) {
                        return facade.apply_bandwidth_precision(
                            std::move(value), decision);
                    },
                    [](const auto& facade, const auto& value) {
                        return facade.next_bandwidth_action(value);
                    }, bandwidth_effect);
    } else {
        return execute_family<
                measurement::DependentLoadLatencyRepetitionState,
                measurement::DependentLoadLatencyCalibrationRequiredAction,
                measurement::DependentLoadLatencyWarmupRequiredAction,
                measurement::DependentLoadLatencyObservationRequiredAction,
                measurement::DependentLoadLatencyObservation>(
                ledger, std::move(state), run,
                statistics_policy, calibration, warmup, storage,
                measurement, statistics,
                [&](const auto& policy) {
                    return run.calibrate_latency(policy);
                },
                [&](const auto& policy, auto work) {
                    return run.warmup_latency(policy, work);
                },
                [](auto& session, const auto& action) {
                    return session.observe_latency(action);
                },
                [](const auto& facade, auto value, const auto& outcome) {
                    return facade.apply_latency_calibration(
                        std::move(value), outcome);
                },
                [](const auto& facade, auto value, const auto& outcome) {
                    return facade.apply_latency_warmup(
                        std::move(value), outcome);
                },
                [](const auto& facade, auto value, const auto& observation,
                   const auto& validity) {
                    return facade.apply_latency_observation(
                        std::move(value), observation, validity);
                },
                [](const auto& facade, auto value, auto decision) {
                    return facade.apply_latency_precision(
                        std::move(value), decision);
                },
                [](const auto& facade, const auto& value) {
                    return facade.next_latency_action(value);
                }, latency_effect);
    }
}

auto close_run_fixed(
    PairwiseRunEvidenceLedger& ledger, PairwiseProductionRun& run,
    PairwiseOperationResult<measurement::RepetitionOutcome>& progression,
    std::size_t expected_workers,
    std::vector<FinalizedEvidenceError>& terminal_secondaries,
    PendingExecutionFacts& pending, bool legacy_hook,
    detail::progress::Publication* publication = nullptr)
    -> std::expected<std::optional<FinalizedPairwiseRunEvidence>, PairwiseRuntimeFailure> {
    pending.finalization.emplace(std::move(run).finalize());
    auto& finalized = *pending.finalization;
    if (legacy_hook)
        if (const auto hook = post_finalize_hook.load()) hook();
    const auto fatal_backend = [&](PairwiseFailureReason reason,
                                   PairwiseFailureContext context) {
        return std::unexpected{runtime_failure(backend_failure(reason, std::move(context)))};
    };
    const auto valid_variant = [expected_workers](const auto& variant) {
        if (variant.workers.size() != expected_workers)
            return false;
        for (std::size_t index = 0; index < expected_workers; ++index)
            if (variant.workers[index].worker_id !=
                core::WorkerId{static_cast<std::uint32_t>(index)})
                return false;
        return true;
    };
    if (!valid_variant(finalized.evidence.first))
        return fatal_backend(
            PairwiseFailureReason::finalization_worker_association,
            worker_context(measurement::ComparisonVariantSlot::first, 0U));
    if (!valid_variant(finalized.evidence.second))
        return fatal_backend(
            PairwiseFailureReason::finalization_worker_association,
            worker_context(measurement::ComparisonVariantSlot::second, 0U));
    const auto progression_errors = progression
        ? std::uint64_t{0}
        : std::uint64_t{1U + progression.error().secondary.size()};
    for (std::size_t index = 0; index < finalized.associations.size(); ++index) {
        const auto& association = finalized.associations[index];
        if (association.slot != measurement::ComparisonVariantSlot::first &&
            association.slot != measurement::ComparisonVariantSlot::second)
            return fatal_backend(
                PairwiseFailureReason::finalization_error_association_slot,
                ordinals(1U, static_cast<std::uint64_t>(association.slot)));
        if (!association.primary_error_ordinal &&
            !association.cleanup_error_ordinal)
            return fatal_backend(
                PairwiseFailureReason::empty_finalization_error_association,
                NoPairwiseFailureContext{});
        if ((association.primary_error_ordinal &&
             *association.primary_error_ordinal >= finalized.errors.size()) ||
            (association.cleanup_error_ordinal &&
             *association.cleanup_error_ordinal >= finalized.errors.size()) ||
            (association.primary_error_ordinal &&
             association.cleanup_error_ordinal &&
             *association.primary_error_ordinal >=
                 *association.cleanup_error_ordinal))
            return fatal_backend(
                PairwiseFailureReason::finalization_error_ordinal,
                ordinals(finalized.errors.size(),
                         association.primary_error_ordinal.value_or(
                             association.cleanup_error_ordinal.value_or(0U))));
        for (std::size_t prior = 0; prior < index; ++prior) {
            const auto& other = finalized.associations[prior];
            if (other.slot == association.slot &&
                other.worker_index == association.worker_index)
                return fatal_backend(
                    PairwiseFailureReason::
                        duplicate_finalization_error_association,
                    ordinals(prior, index));
            for (const auto ordinal :
                 {association.primary_error_ordinal,
                  association.cleanup_error_ordinal})
                if (ordinal &&
                    (other.primary_error_ordinal == ordinal ||
                     other.cleanup_error_ordinal == ordinal))
                    return fatal_backend(
                        PairwiseFailureReason::
                            duplicate_finalization_error_ordinal,
                        ordinals(*ordinal, *ordinal));
        }
        auto& variant =
            association.slot == measurement::ComparisonVariantSlot::first
            ? finalized.evidence.first : finalized.evidence.second;
        if (association.worker_index >= variant.workers.size())
            return fatal_backend(
                PairwiseFailureReason::finalization_error_association,
                worker_context(association.slot, association.worker_index));
        auto& worker = variant.workers[association.worker_index];
        if (worker.primary_error_ordinal || worker.cleanup_error_ordinal)
            return fatal_backend(
                PairwiseFailureReason::
                    duplicate_finalization_error_association,
                ordinals(index, index));
        worker.primary_error_ordinal = association.primary_error_ordinal;
        worker.cleanup_error_ordinal = association.cleanup_error_ordinal;
        if (worker.primary_error_ordinal)
            *worker.primary_error_ordinal += progression_errors;
        if (worker.cleanup_error_ordinal)
            *worker.cleanup_error_ordinal += progression_errors;
    }
    if (publication && publication->pending_count) {
        if (!publication->drain()) return std::optional<FinalizedPairwiseRunEvidence>{};
        if (pending.block) {
            auto appended = detail::PairwiseLedgerPreparedAccess::append_block(ledger, *pending.block);
            if (!appended) return std::unexpected{appended.error()};
            pending.block.reset();
        }
    }
    if (publication && !publication->emit(detail::progress::Kind::finalization, &finalized.evidence))
        return std::optional<FinalizedPairwiseRunEvidence>{};
    auto associated =
        detail::PairwiseLedgerPreparedAccess::associate_finalization(ledger, finalized.evidence);
    if (!associated)
        return std::unexpected{associated.error()};
    if (progression && finalized.errors.empty()) {
        pending.normal.emplace(NormalRunTerminalEvidence{std::move(*progression)});
        if (publication && !publication->emit(detail::progress::Kind::terminal, &*pending.normal))
            return std::optional<FinalizedPairwiseRunEvidence>{};
        auto terminal = detail::PairwiseLedgerPreparedAccess::record_normal_terminal(ledger, *pending.normal);
        if (!terminal)
            return std::unexpected{terminal.error()};
    } else {
        const auto active_block = progression
            ? std::nullopt : progression.error().active_block_id;
        terminal_secondaries.clear();
        const auto needed = (progression ? 0U
                                        : progression.error().secondary.size()) +
                            finalized.errors.size() -
                            (progression ? 1U : 0U);
        if (needed > terminal_secondaries.capacity())
            return std::unexpected{runtime_failure(
                capacity_failure(terminal_secondaries.capacity(), needed))};
        PairwiseEvidenceFailure primary = progression
            ? std::move(finalized.errors.front())
            : std::move(progression.error().primary);

        if (!progression)
            for (auto& error : progression.error().secondary)
                terminal_secondaries.push_back(
                    finalize_failure(std::move(error)));
        const auto begin = progression ? std::size_t{1} : std::size_t{0};
        for (std::size_t index = begin; index < finalized.errors.size(); ++index)
            terminal_secondaries.push_back(
                finalize_failure(std::move(finalized.errors[index])));
        pending.terminal.emplace(OperationalRunTerminalEvidence{
            finalize_failure(std::move(primary)), std::move(terminal_secondaries), active_block});
        if (publication && !publication->emit(detail::progress::Kind::terminal, &*pending.terminal))
            return std::optional<FinalizedPairwiseRunEvidence>{};
        auto terminal = detail::PairwiseLedgerPreparedAccess::record_operational_terminal(ledger, *pending.terminal);
        if (!terminal)
            return std::unexpected{terminal.error()};
    }
    auto result = std::move(ledger).finalize();
    if (!result)
        return std::unexpected{result.error()};
    return std::move(*result);
}

auto legacy_progression(PairwiseOperationResult<std::optional<measurement::RepetitionOutcome>> value)
    -> PairwiseOperationResult<measurement::RepetitionOutcome> {
    if (!value) return std::unexpected{std::move(value.error())};
    if (!*value) std::terminate(); // Legacy execution never installs a progress callback.
    return std::move(**value);
}

auto close_run(
    PairwiseRunEvidenceLedger ledger, PairwiseProductionRun run,
    PairwiseOperationResult<measurement::RepetitionOutcome> progression,
    std::size_t expected_workers,
    std::vector<FinalizedEvidenceError> terminal_secondaries)
    -> std::expected<FinalizedPairwiseRunEvidence, core::Error> {
    PendingExecutionFacts pending;
    auto result = close_run_fixed(ledger, run, progression, expected_workers,
                                  terminal_secondaries, pending, true);
    if (!result) return std::unexpected{typed_fatal_outer_error(result.error())};
    if (!*result) std::terminate();
    return std::move(**result);
}

} // namespace

auto production_calibration_policy(
    const AdmittedPairwiseProductionInputs& inputs)
    -> std::expected<measurement::CalibrationDurationPolicy, core::Error> {
    return calibration_policy(inputs);
}

PairwiseProductionRun::PairwiseProductionRun(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_{std::move(implementation)} {}
PairwiseProductionRun::PairwiseProductionRun(PairwiseProductionRun&&) noexcept = default;
PairwiseProductionRun::~PairwiseProductionRun() noexcept = default;

auto PairwiseProductionRun::calibrate_bandwidth(
    const measurement::CalibrationDurationPolicy& policy) noexcept
    -> PairwiseOperationResult<measurement::SequentialReadBandwidthCalibrationOutcome> {
    if (!implementation_)
        std::terminate();
    return implementation_->calibrate_bandwidth(policy);
}
auto PairwiseProductionRun::calibrate_latency(
    const measurement::CalibrationDurationPolicy& policy) noexcept
    -> PairwiseOperationResult<measurement::DependentLoadLatencyCalibrationOutcome> {
    if (!implementation_)
        std::terminate();
    return implementation_->calibrate_latency(policy);
}
auto PairwiseProductionRun::warmup_bandwidth(
    const measurement::WarmupPolicy& policy,
    kernel::SequentialReadBandwidthPassCount work) noexcept
    -> PairwiseOperationResult<measurement::SequentialReadBandwidthWarmupOutcome> {
    if (!implementation_)
        std::terminate();
    return implementation_->warmup_bandwidth(policy, work);
}
auto PairwiseProductionRun::warmup_latency(
    const measurement::WarmupPolicy& policy,
    kernel::DependentLoadLatencyStepCount work) noexcept
    -> PairwiseOperationResult<measurement::DependentLoadLatencyWarmupOutcome> {
    if (!implementation_)
        std::terminate();
    return implementation_->warmup_latency(policy, work);
}
auto PairwiseProductionRun::observe_bandwidth(
    const measurement::SequentialReadBandwidthObservationRequiredAction& action) noexcept
    -> PairwiseOperationResult<FactualObservationEvidence> {
    if (!implementation_)
        std::terminate();
    return implementation_->observe_bandwidth(action);
}
auto PairwiseProductionRun::observe_latency(
    const measurement::DependentLoadLatencyObservationRequiredAction& action) noexcept
    -> PairwiseOperationResult<FactualObservationEvidence> {
    if (!implementation_)
        std::terminate();
    return implementation_->observe_latency(action);
}
auto PairwiseProductionRun::finalize() && noexcept -> PairwiseFinalizationOutcome {
    if (!implementation_)
        std::terminate();
    return implementation_->finalize_pairwise();
}

void detail::PairwiseRunnerExecutionBackendTestAccess::set_pre_storage_hook(
    void (*hook)()) noexcept {
    pre_storage_hook.store(hook);
}

void detail::PairwiseRunnerExecutionBackendTestAccess::set_post_run_hook(
    void (*hook)()) noexcept {
    post_run_hook.store(hook);
}

void detail::PairwiseRunnerExecutionBackendTestAccess::set_post_finalize_hook(
    void (*hook)()) noexcept {
    post_finalize_hook.store(hook);
}

auto PairwiseRunnerExecutionBackend::execute(
    const AdmittedPairwiseProductionInputs& inputs) const noexcept
    -> std::expected<FinalizedPairwiseRunEvidence, core::Error> {
    try {
        const auto& measurement_source =
            inputs.pairwise_request().first().execution_inputs().measurement();
        auto policies = make_policies(
            measurement_source,
            inputs.pairwise_request().resolved_comparison_order_seed());
        auto calibration = calibration_policy(inputs);
        if (!policies)
            return std::unexpected{policies.error()};
        if (!calibration)
            return std::unexpected{calibration.error()};
        auto [repetition, warmup, order] = *policies;
        auto run_provenance = provenance(inputs);
        if (!run_provenance)
            return std::unexpected{run_provenance.error()};
        const auto maximum_blocks_value =
            measurement_source.repetition_maximum_blocks();
        if constexpr (std::numeric_limits<std::size_t>::max() <
                      std::numeric_limits<std::uint64_t>::max())
            if (maximum_blocks_value >
                std::numeric_limits<std::size_t>::max())
                return std::unexpected{
                    outer_backend_error("admitted_policy")};
        const auto maximum_blocks =
            static_cast<std::size_t>(maximum_blocks_value);
        auto ledger = PairwiseRunEvidenceLedger::create(
            std::move(*run_provenance), inputs.statistics_policy(),
            inputs.monitoring_policy(), repetition, order, maximum_blocks);
        if (!ledger)
            return std::unexpected{ledger.error()};
        const auto expected_workers =
            inputs.pairwise_request().first().workers().size();
        std::optional<PreparedExecutionStorage> storage;
        try {
            if (const auto hook = pre_storage_hook.load())
                hook();
            const auto replicates = inputs.statistics_policy()
                .stopping_bootstrap_policy().replicate_count().value();
            storage.emplace(prepare_execution_storage(
                maximum_blocks, expected_workers,
                static_cast<std::size_t>(replicates)));
        } catch (const std::bad_alloc&) {
            return commit_without_run(
                std::move(*ledger), operation_failure(preparation_failure(
                    PairwiseFailureReason::contained_exception)), {});
        } catch (...) {
            return commit_without_run(
                std::move(*ledger), operation_failure(preparation_failure(
                    PairwiseFailureReason::contained_unknown_failure)), {});
        }
        if (!operations_.context || operations_.prepare_run == nullptr)
            return commit_without_run(
                std::move(*ledger), operation_failure(backend_failure(
                    PairwiseFailureReason::missing_run_factory,
                    NoPairwiseFailureContext{})),
                std::move(storage->terminal_secondaries));
        auto prepared = operations_.prepare_run(operations_.context.get(), inputs);
        if (!prepared)
            return commit_without_run(
                std::move(*ledger), std::move(prepared.error()),
                std::move(storage->terminal_secondaries));
        auto run = std::move(*prepared);
        try {
            if (const auto hook = post_run_hook.load())
                hook();
            const auto experiment_id = core::ExperimentId{
                inputs.pairwise_request().first().request().experiment().index()};
            const auto& kernel_inputs =
                inputs.pairwise_request().first().execution_inputs().kernel();
            const PairwiseMeasurementLeafFacade measurement;
            if (std::holds_alternative<
                    ipc::PlacementBandwidthExecutionInputs>(kernel_inputs)) {
                auto state = measurement.create_bandwidth_repetition(
                    experiment_id, repetition, order,
                    measurement_source.calibration_enabled(), warmup);
                if (!state)
                    return close_run(
                        std::move(*ledger), std::move(run),
                        std::unexpected{operation_failure(state.error())},
                        expected_workers,
                        std::move(storage->terminal_secondaries));
                auto progression = execute_prepared_family(
                    *ledger, std::move(*state), run, inputs.statistics_policy(),
                    *calibration, warmup, *storage);
                return close_run(
                    std::move(*ledger), std::move(run),
                    legacy_progression(std::move(progression)), expected_workers,
                    std::move(storage->terminal_secondaries));
            }
            auto state = measurement.create_latency_repetition(
                experiment_id, repetition, order,
                measurement_source.calibration_enabled(), warmup);
            if (!state)
                return close_run(
                    std::move(*ledger), std::move(run),
                    std::unexpected{operation_failure(state.error())},
                    expected_workers,
                    std::move(storage->terminal_secondaries));
            auto progression = execute_prepared_family(
                    *ledger, std::move(*state), run, inputs.statistics_policy(),
                    *calibration, warmup, *storage);
            return close_run(
                std::move(*ledger), std::move(run), legacy_progression(std::move(progression)),
                expected_workers,
                std::move(storage->terminal_secondaries));
        } catch (const std::exception&) {
            return close_run(
                std::move(*ledger), std::move(run),
                std::unexpected{operation_failure(post_run_failure(true))},
                expected_workers,
                std::move(storage->terminal_secondaries));
        } catch (...) {
            return close_run(
                std::move(*ledger), std::move(run),
                std::unexpected{operation_failure(post_run_failure(false))},
                expected_workers,
                std::move(storage->terminal_secondaries));
        }
    } catch (const std::exception&) {
        return std::unexpected{outer_backend_error("exception")};
    } catch (...) {
        return std::unexpected{
            outer_backend_error("non_standard_exception")};
    }
}


struct detail::PreparedPairwiseRunnerExecution::Storage final {
    using Repetition = std::variant<measurement::SequentialReadBandwidthRepetitionState,
                                    measurement::DependentLoadLatencyRepetitionState>;
    std::unique_ptr<PairwiseProgressState> progress;
    std::optional<PairwisePreparationGraph> graph;
    PairwiseRunEvidenceLedger ledger;
    PreparedExecutionStorage execution;
    StatisticsProductionPolicy statistics;
    measurement::CalibrationDurationPolicy calibration;
    measurement::WarmupPolicy warmup;
    Repetition repetition;
    PairwiseTypedPreparedOwners dormant;
    PreparedPairwiseProductionRun wrapper;
    std::shared_ptr<RunnerCancellation> cancellation;
    ipc::RequestId first_request;
    ipc::RequestId second_request;
    AdmittedPlacementIdentity first_identity;
    AdmittedPlacementIdentity second_identity;
    std::uint64_t order_seed;
    std::size_t workers;
    std::optional<PairwiseProductionRun> run{};
    std::optional<FinalizedPairwiseRunEvidence> finalized{};
    std::optional<PairwiseOperationFailure> preparation_failure{};
    std::optional<PairwiseOperationResult<measurement::RepetitionOutcome>> progression{};
    std::optional<PairwiseRuntimeFailure> fatal{};
    PreparedPairwiseExecutionStage stage{PreparedPairwiseExecutionStage::storage};
    bool run_finalized{};
};

detail::PreparedPairwiseRunnerExecution::PreparedPairwiseRunnerExecution(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}
detail::PreparedPairwiseRunnerExecution::PreparedPairwiseRunnerExecution(
    PreparedPairwiseRunnerExecution&&) noexcept = default;
detail::PreparedPairwiseRunnerExecution::~PreparedPairwiseRunnerExecution() noexcept {
    if (storage_ && storage_->run && !storage_->run_finalized) {
        storage_->execution.pending.finalization.emplace(std::move(*storage_->run).finalize());
        storage_->run_finalized = true;
    }
}

auto detail::PreparedPairwiseRunnerExecution::auxiliary_bytes(bool progress) noexcept -> std::size_t {
    const auto own = sizeof(Storage) + (progress ? sizeof(PairwiseProgressState) : 0U);
    const auto wrapper = PreparedPairwiseProductionRun::auxiliary_bytes();
    if (wrapper > std::numeric_limits<std::size_t>::max() - own) std::terminate();
    return own + wrapper;
}

auto detail::PreparedPairwiseRunnerExecution::prepare(
    const AdmittedPairwiseProductionInputs& inputs,
    PairwiseTypedProductionBindingDependencies dependencies, std::size_t auxiliary_limit, bool progress,
    std::optional<PreparationGraphLimits> graph_limits) noexcept
    -> PairwiseOperationResult<PreparedPairwiseRunnerExecution> {
    if (dependencies.policy == stage_b_policy::Selection::stage_b_v1 &&
        (!stage_b_policy::admit(inputs) || (graph_limits && !stage_b_policy::admit(*graph_limits))))
        return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    if (auxiliary_limit < auxiliary_bytes(progress))
        return std::unexpected{operation_failure(capacity_failure(auxiliary_limit, auxiliary_bytes(progress)))};
    if (!dependencies.shared_cancellation)
        return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    try {
        const auto& request = inputs.pairwise_request();
        const auto& source = request.first().execution_inputs().measurement();
        auto policies = make_policies(source, request.resolved_comparison_order_seed());
        auto calibration = calibration_policy(inputs);
        auto provenance_value = provenance(inputs);
        if (!policies || !calibration || !provenance_value)
            return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
                PairwiseFailureReason::contained_exception))};
        const auto [repetition, warmup, order] = *policies;
        const auto count = source.repetition_maximum_blocks();
        if (count > std::numeric_limits<std::size_t>::max())
            return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
                PairwiseFailureReason::contained_exception))};
        const auto blocks = static_cast<std::size_t>(count);
        const auto workers = request.first().workers().size();
        auto ledger = PairwiseRunEvidenceLedger::create(
            std::move(*provenance_value), inputs.statistics_policy(),
            inputs.monitoring_policy(), repetition, order, blocks);
        if (!ledger)
            return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
                PairwiseFailureReason::contained_exception))};
        const auto replicates = inputs.statistics_policy().stopping_bootstrap_policy().replicate_count().value();
        if (replicates > std::numeric_limits<std::size_t>::max())
            return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
                PairwiseFailureReason::contained_exception))};
        auto execution = prepare_execution_storage(blocks, workers, static_cast<std::size_t>(replicates));
        const PairwiseMeasurementLeafFacade measurement;
        const auto experiment = core::ExperimentId{request.first().request().experiment().index()};
        auto state = [&]() -> std::expected<Storage::Repetition, PairwiseEvidenceFailure> {
            if (std::holds_alternative<ipc::PlacementBandwidthExecutionInputs>(
                    request.first().execution_inputs().kernel())) {
                auto value = measurement.create_bandwidth_repetition(
                    experiment, repetition, order, source.calibration_enabled(), warmup);
                if (!value) return std::unexpected{value.error()};
                return Storage::Repetition{std::move(*value)};
            }
            auto value = measurement.create_latency_repetition(
                experiment, repetition, order, source.calibration_enabled(), warmup);
            if (!value) return std::unexpected{value.error()};
            return Storage::Repetition{std::move(*value)};
        }();
        if (!state) return std::unexpected{operation_failure(state.error())};
        auto wrapper = PreparedPairwiseProductionRun::prepare();
        if (!wrapper) return std::unexpected{std::move(wrapper.error())};
        const PairwiseTypedPreOwnerDependencies borrowed{
            &dependencies.capacity_producer, dependencies.platform,
            dependencies.temperature_operations, dependencies.frequency_operations,
            dependencies.kernel, dependencies.worker, dependencies.shared_cancellation, dependencies.policy};
        auto typed = prepare_pairwise_typed_pre_owner(inputs, borrowed, graph_limits);
        if (!typed) return std::unexpected{std::move(typed.error())};
        auto graph = typed->preparation_graph_
            ? std::optional{typed->preparation_graph_->retain()} : std::nullopt;
        auto progress_state = progress ? std::make_unique<PairwiseProgressState>() : nullptr;
        auto dormant = prepare_pairwise_typed_owner_transaction_storage(std::move(*typed), progress_state.get());
        if (!dormant) return std::unexpected{std::move(dormant.error())};
        auto storage = std::make_unique<Storage>(Storage{
            std::move(progress_state), std::move(graph), std::move(*ledger), std::move(execution), inputs.statistics_policy(),
            *calibration, warmup, std::move(*state), std::move(*dormant),
            std::move(*wrapper), std::move(dependencies.shared_cancellation),
            request.first().request_id(), request.second().request_id(),
            request.first().placement_identity(), request.second().placement_identity(),
            request.resolved_comparison_order_seed(), workers});
        return PreparedPairwiseRunnerExecution{std::move(storage)};
    } catch (...) {
        return std::unexpected{operation_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    }
}

auto detail::PreparedPairwiseRunnerExecution::preparation_graph() const noexcept -> const PairwisePreparationGraph* {
    return storage_ && storage_->graph ? &*storage_->graph : nullptr;
}

auto detail::PreparedPairwiseRunnerExecution::progress_state() noexcept -> PairwiseProgressState* {
    return storage_ ? storage_->progress.get() : nullptr;
}

auto detail::PreparedPairwiseRunnerExecution::acquire() noexcept
    -> std::expected<void, PairwiseEvidenceFailure> {
    if (!storage_ || storage_->stage != PreparedPairwiseExecutionStage::storage)
        return std::unexpected{::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception)};
    auto& value = *storage_;
    value.stage = PreparedPairwiseExecutionStage::failed;
    auto owners = acquire_pairwise_typed_owners(std::move(value.dormant));
    if (!owners)
        value.preparation_failure.emplace(materialize_pairwise_owner_preparation_failure(
            std::move(owners.error()).release()));
    else {
        auto core = bind_pairwise_typed_owner_run(std::move(*owners));
        if (!core) value.preparation_failure.emplace(std::move(core.error()));
        else {
            if (value.graph && !value.graph->transition(PreparationGraphStage::bound)) std::terminate();
            auto bound = std::move(value.wrapper).bind(std::move(*core));
            if (!bound) value.preparation_failure.emplace(std::move(bound.error()));
            else {
                value.run.emplace(std::move(*bound));
                if (value.graph && !value.graph->transition(PreparationGraphStage::published)) std::terminate();
                value.stage = PreparedPairwiseExecutionStage::acquired;
                return {};
            }
        }
    }
    if (value.graph) value.graph->retire();
    const auto primary = value.preparation_failure->primary;
    auto committed = commit_without_run_fixed(value.ledger, *value.preparation_failure,
        value.execution.terminal_secondaries, value.execution.pending);
    if (committed) value.finalized.emplace(std::move(*committed));
    else value.fatal.emplace(committed.error());
    return std::unexpected{primary};
}

auto detail::PreparedPairwiseRunnerExecution::execute(void* finalization_context,
    void (*before_finalization)(void*) noexcept) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    auto result = execute_impl({},finalization_context,before_finalization);
    if (!result) return std::unexpected{result.error()};
    if (!*result) std::terminate(); // No callback exists on the legacy entry.
    return {};
}

auto detail::PreparedPairwiseRunnerExecution::execute_progress(progress::Callback callback,
    void* finalization_context, void (*before_finalization)(void*) noexcept) noexcept
    -> std::expected<bool, PairwiseRuntimeFailure> {
    if (!storage_ || !storage_->progress || callback.consume == nullptr)
        return std::unexpected{runtime_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    return execute_impl(callback,finalization_context,before_finalization);
}

auto detail::PreparedPairwiseRunnerExecution::progress_failure() const noexcept -> std::optional<progress::Error> {
    return storage_ ? storage_->execution.publication.failure : std::nullopt;
}

auto detail::PreparedPairwiseRunnerExecution::execute_impl(progress::Callback callback,
    void* finalization_context, void (*before_finalization)(void*) noexcept) noexcept
    -> std::expected<bool, PairwiseRuntimeFailure> {
    if (!storage_ || storage_->stage != PreparedPairwiseExecutionStage::acquired)
        return std::unexpected{runtime_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    auto& value = *storage_;
    value.stage = PreparedPairwiseExecutionStage::failed;
    value.execution.publication = {value.progress.get(),callback,0,false,std::nullopt};
    if (value.graph && !value.execution.publication.emit(progress::Kind::preparation, &*value.graph)) {
        if (before_finalization) before_finalization(finalization_context);
        value.execution.pending.finalization.emplace(std::move(*value.run).finalize());
        value.run_finalized = true;
        value.stage = PreparedPairwiseExecutionStage::interrupted;
        return false;
    }
    try {
        auto result = std::visit([&](auto& state) {
            return execute_prepared_family(value.ledger, std::move(state), *value.run,
                value.statistics, value.calibration, value.warmup, value.execution);
        }, value.repetition);
        if (result && !*result) {
            if (before_finalization) before_finalization(finalization_context);
            value.execution.pending.finalization.emplace(std::move(*value.run).finalize());
            value.run_finalized = true;
            value.stage = PreparedPairwiseExecutionStage::interrupted;
            return false;
        }
        if (result) value.progression.emplace(std::move(**result));
        else value.progression.emplace(std::unexpected{std::move(result.error())});
    } catch (const std::exception&) {
        value.progression.emplace(std::unexpected{operation_failure(post_run_failure(true))});
    } catch (...) {
        value.progression.emplace(std::unexpected{operation_failure(post_run_failure(false))});
    }
    // Fixed orchestration notification marks the actual finalization boundary.
    if (before_finalization) before_finalization(finalization_context);
    // No throwing hooks or allocating Error renderers enter this active path.
    value.run_finalized = true;
    auto closed = close_run_fixed(value.ledger, *value.run, *value.progression, value.workers,
        value.execution.terminal_secondaries, value.execution.pending, false, &value.execution.publication);
    if (!closed) {
        value.fatal.emplace(closed.error());
        return std::unexpected{closed.error()};
    }
    if (!*closed) {
        value.stage = PreparedPairwiseExecutionStage::interrupted;
        return false;
    }
    value.finalized.emplace(std::move(**closed));
    value.stage = PreparedPairwiseExecutionStage::finalized;
    return true;
}

auto detail::PreparedPairwiseRunnerExecution::finalize_without_execution() noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (!storage_ || storage_->stage != PreparedPairwiseExecutionStage::acquired)
        return std::unexpected{runtime_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    storage_->execution.pending.finalization.emplace(std::move(*storage_->run).finalize());
    storage_->run_finalized = true;
    storage_->stage = PreparedPairwiseExecutionStage::finalized;
    return {};
}

auto detail::PreparedPairwiseRunnerExecution::stage() const noexcept -> PreparedPairwiseExecutionStage {
    return storage_ ? storage_->stage : PreparedPairwiseExecutionStage::failed;
}
auto detail::PreparedPairwiseRunnerExecution::matches(
    const AdmittedPairwiseProductionInputs& inputs) const noexcept -> bool {
    if (!storage_) return false;
    const auto& pair = inputs.pairwise_request();
    return storage_->first_request == pair.first().request_id() &&
        storage_->second_request == pair.second().request_id() &&
        storage_->first_identity == pair.first().placement_identity() &&
        storage_->second_identity == pair.second().placement_identity() &&
        storage_->order_seed == pair.resolved_comparison_order_seed() &&
        storage_->statistics == inputs.statistics_policy();
}
auto detail::PreparedPairwiseRunnerExecution::cancellation() const noexcept
    -> const std::shared_ptr<RunnerCancellation>& {
    if (!storage_) std::terminate();
    return storage_->cancellation;
}
auto detail::PreparedPairwiseRunnerExecution::finalized_evidence() const noexcept
    -> const FinalizedPairwiseRunEvidence* {
    return storage_ && storage_->finalized ? &*storage_->finalized : nullptr;
}
auto detail::PreparedPairwiseRunnerExecution::take_finalized_evidence() noexcept
    -> std::expected<FinalizedPairwiseRunEvidence, PairwiseRuntimeFailure> {
    if (!storage_ || !storage_->finalized || storage_->stage == PreparedPairwiseExecutionStage::transferred)
        return std::unexpected{runtime_failure(::epycbench::runtime::preparation_failure(
            PairwiseFailureReason::contained_exception))};
    storage_->stage = PreparedPairwiseExecutionStage::transferred;
    if (storage_->progress) storage_->progress->close_receipts();
    auto result = std::move(*storage_->finalized);
    storage_->finalized.reset();
    return result;
}
auto detail::PreparedPairwiseRunnerExecution::ledger() const noexcept -> const PairwiseRunEvidenceLedger* {
    return storage_ && !storage_->finalized && storage_->stage != PreparedPairwiseExecutionStage::transferred
        ? &storage_->ledger : nullptr;
}
auto detail::PreparedPairwiseRunnerExecution::pending_block() const noexcept -> const PairwiseBlockAttemptEvidence* {
    return storage_ && storage_->execution.pending.block ? &*storage_->execution.pending.block : nullptr;
}
auto detail::PreparedPairwiseRunnerExecution::pending_finalization() const noexcept -> const PairwiseFinalizationOutcome* {
    return storage_ && storage_->execution.pending.finalization ? &*storage_->execution.pending.finalization : nullptr;
}
auto detail::PreparedPairwiseRunnerExecution::pending_terminal() const noexcept -> const OperationalRunTerminalEvidence* {
    return storage_ && storage_->execution.pending.terminal ? &*storage_->execution.pending.terminal : nullptr;
}
auto detail::PreparedPairwiseRunnerExecution::preparation_failure() const noexcept -> const PairwiseOperationFailure* {
    return storage_ && storage_->preparation_failure ? &*storage_->preparation_failure : nullptr;
}
auto detail::PreparedPairwiseRunnerExecution::fatal_failure() const noexcept -> const PairwiseRuntimeFailure* {
    return storage_ && storage_->fatal ? &*storage_->fatal : nullptr;
}

} // namespace epycbench::runtime
