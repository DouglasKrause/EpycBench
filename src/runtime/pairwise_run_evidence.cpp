#include "epycbench/runtime/pairwise_run_evidence.hpp"
#include "runtime/pairwise_block_association.hpp"

#include <exception>
#include <ranges>
#include <utility>

namespace epycbench::runtime {
namespace {

auto preledger_error(std::string reason) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{
                "runtime.evidence_ledger.inconsistent_evidence"},
            .message = "Typed pairwise evidence ledger rejected preparation",
            .operation = "pairwise_run_evidence",
            .os_error = std::nullopt,
            .context = {{"reason", std::move(reason)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto ledger_failure(PairwiseFailureCode code, PairwiseFailureReason reason,
                    PairwiseFailureContext context) noexcept
    -> PairwiseRuntimeFailure {
    auto value = PairwiseRuntimeFailure::create(
        code, PairwiseFailureOperation::pairwise_run_evidence, reason,
        std::move(context));
    if (!value)
        std::terminate();
    return *value;
}

auto invalid_transition(PairwiseFailureReason reason,
                        PairwiseFailureContext context) noexcept
    -> PairwiseRuntimeFailure {
    return ledger_failure(
        PairwiseFailureCode::runtime_evidence_ledger_invalid_transition,
        reason, std::move(context));
}

auto inconsistent(PairwiseFailureReason reason,
                  PairwiseFailureContext context) noexcept
    -> PairwiseRuntimeFailure {
    return ledger_failure(
        PairwiseFailureCode::runtime_evidence_ledger_inconsistent_evidence,
        reason, std::move(context));
}

auto counts(std::uint64_t expected, std::uint64_t observed) noexcept
    -> PairwiseFailureContext {
    return ExpectedObservedCountContext{
        PairwiseCount{expected}, PairwiseCount{observed}};
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

auto observation_identity(const measurement::MeasurementObservation& observation)
    noexcept -> core::ObservationId {
    return std::visit(
        [](const auto& value) { return value.observation_id(); }, observation);
}

auto observation_block(const measurement::MeasurementObservation& observation)
    noexcept -> kernel::DependentLoadLatencyBlockId {
    return std::visit(
        [](const auto& value) { return value.block_id(); }, observation);
}

auto check_outcome(const measurement::ObservationValidityEvidence& validity,
                   measurement::ObservationValidityCheckId id) noexcept
    -> std::optional<measurement::ObservationValidityCheckOutcome> {
    for (const auto& check : validity.checks())
        if (check.id() == id)
            return check.outcome();
    return std::nullopt;
}

auto placement_outcome(
    std::span<const measurement::WorkerPlacementValidityEvidence> values)
    noexcept -> measurement::ObservationValidityCheckOutcome {
    const auto has = [&](auto state) {
        return std::ranges::any_of(values, [state](const auto& value) {
            return value.state() == state;
        });
    };
    if (has(measurement::WorkerPlacementValidityState::not_qualified))
        return measurement::ObservationValidityCheckOutcome::fail;
    if (has(measurement::WorkerPlacementValidityState::unavailable))
        return measurement::ObservationValidityCheckOutcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& value) {
            return value.state() ==
                   measurement::WorkerPlacementValidityState::not_applicable;
        }))
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    return measurement::ObservationValidityCheckOutcome::pass;
}

auto migration_outcome(
    std::span<const measurement::WorkerMigrationValidityEvidence> values)
    noexcept -> measurement::ObservationValidityCheckOutcome {
    const auto has = [&](auto state) {
        return std::ranges::any_of(values, [state](const auto& value) {
            return value.state() == state;
        });
    };
    if (has(measurement::WorkerMigrationValidityState::migrated))
        return measurement::ObservationValidityCheckOutcome::fail;
    if (has(measurement::WorkerMigrationValidityState::unavailable))
        return measurement::ObservationValidityCheckOutcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& value) {
            return value.state() ==
                   measurement::WorkerMigrationValidityState::not_applicable;
        }))
        return measurement::ObservationValidityCheckOutcome::not_applicable;
    return measurement::ObservationValidityCheckOutcome::pass;
}

} // namespace

PairwiseRunEvidenceLedger::PairwiseRunEvidenceLedger(
    PairwiseRunProvenance provenance,
    StatisticsProductionPolicy statistics_policy,
    monitoring::MonitoringExecutionPolicy monitoring_policy,
    measurement::RepetitionPolicy repetition_policy,
    measurement::ComparisonOrderPolicy comparison_order,
    std::vector<PairwiseBlockAttemptEvidence> blocks,
    std::vector<RunnerStoppingEvaluationEvidence> stopping) noexcept
    : provenance_{std::move(provenance)},
      statistics_policy_{std::move(statistics_policy)},
      monitoring_policy_{std::move(monitoring_policy)},
      repetition_policy_{std::move(repetition_policy)},
      comparison_order_{std::move(comparison_order)},
      blocks_{std::move(blocks)}, stopping_{std::move(stopping)} {}

auto PairwiseRunEvidenceLedger::create(
    PairwiseRunProvenance provenance,
    StatisticsProductionPolicy statistics_policy,
    monitoring::MonitoringExecutionPolicy monitoring_policy,
    measurement::RepetitionPolicy repetition_policy,
    measurement::ComparisonOrderPolicy comparison_order,
    std::size_t maximum_blocks) noexcept
    -> std::expected<PairwiseRunEvidenceLedger, core::Error> {
    try {
        if (provenance.scenario_profile.empty() ||
            provenance.scenario_digest.empty() ||
            provenance.resolved_plan_digest.empty() ||
            provenance.workload_id.empty() ||
            provenance.first_variant_id.empty() ||
            provenance.second_variant_id.empty() ||
            provenance.first_variant_id == provenance.second_variant_id)
            return std::unexpected{preledger_error("invalid_provenance")};
        std::vector<PairwiseBlockAttemptEvidence> blocks;
        std::vector<RunnerStoppingEvaluationEvidence> stopping;
        blocks.reserve(maximum_blocks);
        stopping.reserve(maximum_blocks);
        return PairwiseRunEvidenceLedger{
            std::move(provenance), std::move(statistics_policy),
            std::move(monitoring_policy), std::move(repetition_policy),
            std::move(comparison_order), std::move(blocks),
            std::move(stopping)};
    } catch (const std::exception&) {
        return std::unexpected{preledger_error("allocation")};
    } catch (...) {
        return std::unexpected{preledger_error("non_standard_exception")};
    }
}

auto detail::PreparedPairwiseLedgerStorage::prepare(
    std::size_t maximum_blocks) noexcept
    -> std::optional<PreparedPairwiseLedgerStorage> {
    try {
        std::vector<PairwiseBlockAttemptEvidence> blocks;
        std::vector<RunnerStoppingEvaluationEvidence> stopping;
        blocks.reserve(maximum_blocks);
        stopping.reserve(maximum_blocks);
        return PreparedPairwiseLedgerStorage{std::move(blocks), std::move(stopping)};
    } catch (...) {
        return std::nullopt;
    }
}

auto detail::PairwiseLedgerPreparedAccess::create(
    PairwiseRunProvenance provenance,
    StatisticsProductionPolicy statistics_policy,
    monitoring::MonitoringExecutionPolicy monitoring_policy,
    measurement::RepetitionPolicy repetition_policy,
    measurement::ComparisonOrderPolicy comparison_order,
    PreparedPairwiseLedgerStorage storage) noexcept
    -> std::optional<PairwiseRunEvidenceLedger> {
    if (provenance.scenario_profile.empty() || provenance.scenario_digest.empty() ||
        provenance.resolved_plan_digest.empty() || provenance.workload_id.empty() ||
        provenance.first_variant_id.empty() || provenance.second_variant_id.empty() ||
        provenance.first_variant_id == provenance.second_variant_id ||
        !storage.blocks_.empty() || !storage.stopping_.empty() ||
        repetition_policy.maximum_blocks() > storage.blocks_.capacity() ||
        repetition_policy.maximum_blocks() > storage.stopping_.capacity())
        return std::nullopt;
    return PairwiseRunEvidenceLedger{
        std::move(provenance), std::move(statistics_policy),
        std::move(monitoring_policy), std::move(repetition_policy),
        std::move(comparison_order), std::move(storage.blocks_),
        std::move(storage.stopping_)};
}

auto detail::PairwiseLedgerPreparedAccess::append_reconstructed_stopping_evaluation(
    PairwiseRunEvidenceLedger& ledger,
    RunnerStoppingEvaluationEvidence evidence) noexcept -> bool {
    if (ledger.terminal_ || ledger.finalized_ ||
        ledger.stopping_.size() == ledger.stopping_.capacity() ||
        (!ledger.stopping_.empty() &&
         evidence.completed_blocks <= ledger.stopping_.back().completed_blocks))
        return false;
    const auto boundary = evidence.boundary_block_id;
    if (std::ranges::none_of(ledger.blocks_, [boundary](const auto& block) {
            return block.plan.block_id() == boundary;
        }))
        return false;
    ledger.stopping_.push_back(std::move(evidence));
    return true;
}

auto PairwiseRunEvidenceLedger::associate_calibration(
    PairwiseCalibrationEvidence&& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (terminal_ || finalized_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::terminal,
            LifecyclePhaseContext{PairwiseLifecyclePhase::active})};
    if (calibration_ || !blocks_.empty())
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::calibration_order,
            ordinals(0U, calibration_ ? 1U : blocks_.size()))};
    calibration_.emplace(std::move(evidence));
    return {};
}

auto PairwiseRunEvidenceLedger::associate_warmup(
    PairwiseWarmupEvidence&& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (terminal_ || finalized_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::terminal,
            LifecyclePhaseContext{PairwiseLifecyclePhase::active})};
    if (warmup_ || !blocks_.empty())
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::warmup_order,
            ordinals(0U, warmup_ ? 1U : blocks_.size()))};
    warmup_.emplace(std::move(evidence));
    return {};
}

auto detail::validate_pairwise_block_association(
    PairwiseBlockAssociationView evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    const auto block_id = evidence.plan.block_id();
    const auto& planned = evidence.plan.observations();
    if (evidence.count > planned.size())
        return std::unexpected{inconsistent(
            PairwiseFailureReason::slot_count,
            counts(planned.size(), evidence.count))};
    for (std::size_t index = 0; index < evidence.count; ++index) {
        const auto& factual = evidence.observation(evidence.owner, index);
        const bool projections_match = std::visit(
            [&](const auto& observation) {
                const auto workers = observation.workers();
                if (factual.placements.size() != workers.size() ||
                    factual.migrations.size() != workers.size())
                    return false;
                for (std::size_t worker = 0; worker < workers.size(); ++worker)
                    if (factual.placements[worker].worker_id() !=
                            workers[worker].worker_id() ||
                        factual.migrations[worker].worker_id() !=
                            workers[worker].worker_id())
                        return false;
                return true;
            }, factual.observation);
        const bool outcomes_match =
            check_outcome(factual.validity,
                          measurement::ObservationValidityCheckId::placement) ==
                placement_outcome(factual.placements) &&
            check_outcome(factual.validity,
                          measurement::ObservationValidityCheckId::migration) ==
                migration_outcome(factual.migrations);
        if (!(factual.slot == planned[index]) ||
            factual.observation_id != observation_identity(factual.observation) ||
            factual.validity.observation_id() != factual.observation_id ||
            observation_block(factual.observation) != block_id ||
            !projections_match || !outcomes_match)
            return std::unexpected{inconsistent(
                PairwiseFailureReason::observation_association,
                ordinals(index, factual.slot.slot_index()))};
    }
    if (evidence.completion ==
        BlockAttemptCompletion::operationally_incomplete) {
        if (!evidence.failed ||
            evidence.count >= planned.size())
            return std::unexpected{inconsistent(
                PairwiseFailureReason::incomplete_shape,
                counts(evidence.count + 1U, planned.size()))};
        const auto expected = planned[evidence.count];
        if (!(evidence.failed->slot == expected))
            return std::unexpected{inconsistent(
                PairwiseFailureReason::failed_slot_association,
                ordinals(expected.slot_index(),
                         evidence.failed->slot.slot_index()))};
    } else {
        if (evidence.count != planned.size() ||
            evidence.failed)
            return std::unexpected{inconsistent(
                PairwiseFailureReason::complete_shape,
                counts(planned.size(), evidence.count))};
        bool all_valid = true;
        for (std::size_t index{}; index < evidence.count; ++index)
            all_valid = all_valid && evidence.observation(evidence.owner, index).validity.disposition() ==
                measurement::ObservationValidityDisposition::valid;
        if (all_valid != (evidence.completion ==
                          BlockAttemptCompletion::complete_valid))
            return std::unexpected{inconsistent(
                PairwiseFailureReason::completion_classification,
                LifecyclePhaseContext{PairwiseLifecyclePhase::active})};
    }
    return {};
}

auto detail::validate_pairwise_block_association(const PairwiseBlockAttemptEvidence& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    return validate_pairwise_block_association(PairwiseBlockAssociationView{
        evidence.plan, evidence.observations.size(), &evidence,
        [](const void* owner, std::size_t index) noexcept -> const FactualObservationEvidence& {
            return static_cast<const PairwiseBlockAttemptEvidence*>(owner)->observations[index];
        }, evidence.failed_observation ? &*evidence.failed_observation : nullptr, evidence.completion});
}

auto PairwiseRunEvidenceLedger::append_block(
    PairwiseBlockAttemptEvidence evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    return append_block_pending(evidence);
}

auto PairwiseRunEvidenceLedger::append_block_pending(
    PairwiseBlockAttemptEvidence& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (terminal_ || finalized_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::terminal,
            LifecyclePhaseContext{PairwiseLifecyclePhase::active})};
    const auto block_id = evidence.plan.block_id();
    if (!blocks_.empty() &&
        block_id.value() <= blocks_.back().plan.block_id().value())
        return std::unexpected{inconsistent(
            PairwiseFailureReason::block_order,
            ordinals(blocks_.back().plan.block_id().value() + 1U,
                     block_id.value()))};
    if (blocks_.size() == blocks_.capacity())
        return std::unexpected{inconsistent(
            PairwiseFailureReason::slot_count,
            counts(blocks_.capacity(), blocks_.size() + 1U))};
    auto association = detail::validate_pairwise_block_association(evidence);
    if (!association) return std::unexpected{association.error()};
    blocks_.push_back(std::move(evidence));
    return {};
}

auto PairwiseRunEvidenceLedger::append_stopping_evaluation(
    RunnerStoppingEvaluationEvidence evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    return append_stopping_evaluation_pending(evidence);
}

auto PairwiseRunEvidenceLedger::append_stopping_evaluation_pending(
    RunnerStoppingEvaluationEvidence& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (terminal_ || finalized_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::terminal,
            LifecyclePhaseContext{PairwiseLifecyclePhase::active})};
    if (blocks_.empty() ||
        evidence.boundary_block_id != blocks_.back().plan.block_id())
        return std::unexpected{inconsistent(
            PairwiseFailureReason::stopping_boundary,
            ordinals(blocks_.empty() ? 0U :
                         blocks_.back().plan.block_id().value(),
                     evidence.boundary_block_id.value()))};
    if ((!stopping_.empty() &&
         evidence.completed_blocks <= stopping_.back().completed_blocks) ||
        stopping_.size() == stopping_.capacity())
        return std::unexpected{inconsistent(
            PairwiseFailureReason::stopping_order,
            ordinals(stopping_.empty() ? 1U :
                         stopping_.back().completed_blocks + 1U,
                     evidence.completed_blocks))};
    stopping_.push_back(std::move(evidence));
    return {};
}

auto PairwiseRunEvidenceLedger::record_normal_terminal(
    NormalRunTerminalEvidence evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    return record_normal_terminal_pending(evidence);
}

auto PairwiseRunEvidenceLedger::record_normal_terminal_pending(
    NormalRunTerminalEvidence& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (terminal_ || finalized_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::duplicate_terminal,
            LifecyclePhaseContext{PairwiseLifecyclePhase::finalization})};
    terminal_.emplace(std::move(evidence));
    return {};
}

auto PairwiseRunEvidenceLedger::record_operational_terminal(
    OperationalRunTerminalEvidence evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    return record_operational_terminal_pending(evidence);
}

auto PairwiseRunEvidenceLedger::record_operational_terminal_pending(
    OperationalRunTerminalEvidence& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (terminal_ || finalized_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::duplicate_terminal,
            LifecyclePhaseContext{PairwiseLifecyclePhase::finalization})};
    if (evidence.active_block_id &&
        (blocks_.empty() ||
         blocks_.back().completion !=
             BlockAttemptCompletion::operationally_incomplete ||
         blocks_.back().plan.block_id() != *evidence.active_block_id))
        return std::unexpected{inconsistent(
            PairwiseFailureReason::active_block,
            ordinals(blocks_.empty() ? 0U :
                         blocks_.back().plan.block_id().value(),
                     evidence.active_block_id->value()))};
    terminal_.emplace(std::move(evidence));
    return {};
}

auto PairwiseRunEvidenceLedger::associate_finalization(
    PairwiseFinalizationEvidence evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    return associate_finalization_pending(evidence);
}

auto PairwiseRunEvidenceLedger::associate_finalization_pending(
    PairwiseFinalizationEvidence& evidence) noexcept
    -> std::expected<void, PairwiseRuntimeFailure> {
    if (finalized_ || terminal_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::finalization_order,
            ordinals(0U, terminal_ ? 1U : 0U))};
    if (finalization_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::duplicate_finalization,
            ordinals(0U, 1U))};
    if (evidence.first.slot != measurement::ComparisonVariantSlot::first ||
        evidence.second.slot != measurement::ComparisonVariantSlot::second)
        return std::unexpected{inconsistent(
            PairwiseFailureReason::finalization_variant_slots,
            counts(2U, static_cast<std::uint64_t>(
                evidence.first.slot == measurement::ComparisonVariantSlot::first) +
                static_cast<std::uint64_t>(
                evidence.second.slot == measurement::ComparisonVariantSlot::second)))};
    const auto validate = [&](const FinalizedVariantEvidence& variant) {
        for (std::size_t index = 0; index < variant.workers.size(); ++index) {
            if (index != 0U &&
                variant.workers[index - 1U].worker_id.value() >=
                    variant.workers[index].worker_id.value())
                return false;
            if (const auto* available = variant.workers[index].region.available();
                available != nullptr && available->request().worker_id() !=
                                            variant.workers[index].worker_id)
                return false;
        }
        return true;
    };
    if (evidence.first.workers.size() != evidence.second.workers.size() ||
        !validate(evidence.first) || !validate(evidence.second))
        return std::unexpected{inconsistent(
            PairwiseFailureReason::finalization_worker_association,
            worker_context(measurement::ComparisonVariantSlot::first, 0U))};
    for (std::size_t index = 0; index < evidence.first.workers.size(); ++index)
        if (evidence.first.workers[index].worker_id !=
            evidence.second.workers[index].worker_id)
            return std::unexpected{inconsistent(
                PairwiseFailureReason::finalization_worker_association,
                worker_context(measurement::ComparisonVariantSlot::second,
                               index))};
    finalization_.emplace(std::move(evidence));
    return {};
}

auto PairwiseRunEvidenceLedger::finalize() && noexcept
    -> std::expected<FinalizedPairwiseRunEvidence,
                     PairwiseRuntimeFailure> {
    if (finalized_ || !terminal_)
        return std::unexpected{invalid_transition(
            PairwiseFailureReason::finalize_state,
            LifecyclePhaseContext{PairwiseLifecyclePhase::finalization})};
    if (const auto* normal =
            std::get_if<NormalRunTerminalEvidence>(&*terminal_)) {
        if (!finalization_)
            return std::unexpected{inconsistent(
                PairwiseFailureReason::missing_finalization,
                LifecyclePhaseContext{PairwiseLifecyclePhase::finalization})};
        std::uint64_t completed{};
        std::uint64_t complete_valid{};
        for (const auto& block : blocks_) {
            if (block.completion !=
                BlockAttemptCompletion::operationally_incomplete)
                ++completed;
            if (block.completion == BlockAttemptCompletion::complete_valid)
                ++complete_valid;
        }
        const auto& outcome = normal->outcome;
        if (outcome.experiment_id() != provenance_.experiment_id ||
            outcome.policy() != repetition_policy_ ||
            outcome.completed_blocks() != completed ||
            outcome.complete_valid_blocks() != complete_valid ||
            outcome.valid_first_variant_observations() != complete_valid * 2U ||
            outcome.valid_second_variant_observations() != complete_valid * 2U)
            return std::unexpected{inconsistent(
                PairwiseFailureReason::normal_terminal_counts,
                counts(completed, outcome.completed_blocks()))};
    }
    if (finalization_) {
        const auto* operational =
            std::get_if<OperationalRunTerminalEvidence>(
                &*terminal_);
        const auto error_count = operational == nullptr
            ? std::uint64_t{0}
            : std::uint64_t{1U + operational->secondary_errors.size()};
        for (const auto* variant :
             {&finalization_->first, &finalization_->second}) {
            for (std::size_t index = 0; index < variant->workers.size();
                 ++index) {
                const auto& worker = variant->workers[index];
                const auto check = [&](const std::optional<std::uint64_t>& ordinal) {
                    if (!ordinal)
                        return true;
                    if (*ordinal >= error_count)
                        return false;
                    std::size_t occurrences{};
                    for (const auto* candidate_variant :
                         {&finalization_->first, &finalization_->second})
                        for (const auto& candidate : candidate_variant->workers) {
                            if (candidate.primary_error_ordinal == ordinal)
                                ++occurrences;
                            if (candidate.cleanup_error_ordinal == ordinal)
                                ++occurrences;
                        }
                    return occurrences == 1U;
                };
                if (!check(worker.primary_error_ordinal) ||
                    !check(worker.cleanup_error_ordinal))
                    return std::unexpected{inconsistent(
                        PairwiseFailureReason::finalization_error_ordinal,
                        ordinals(error_count,
                            worker.primary_error_ordinal.value_or(
                                worker.cleanup_error_ordinal.value_or(0U))))};
                if (const auto* available = worker.region.available()) {
                    const auto role = variant->slot;
                    if (available->disposition() ==
                            memory::RegionFinalizationDisposition::succeeded &&
                        (worker.primary_error_ordinal ||
                         worker.cleanup_error_ordinal))
                        return std::unexpected{inconsistent(
                            PairwiseFailureReason::successful_worker_error,
                            worker_context(role, index))};
                    if (available->disposition() ==
                            memory::RegionFinalizationDisposition::failed &&
                        !worker.primary_error_ordinal)
                        return std::unexpected{inconsistent(
                            PairwiseFailureReason::failed_worker_without_error,
                            worker_context(role, index))};
                }
            }
        }
    }
    finalized_ = true;
    return FinalizedPairwiseRunEvidence{
        std::move(provenance_), std::move(statistics_policy_),
        std::move(monitoring_policy_), std::move(repetition_policy_),
        std::move(comparison_order_), std::move(calibration_),
        std::move(warmup_), std::move(blocks_), std::move(stopping_),
        std::move(*terminal_), std::move(finalization_)};
}

} // namespace epycbench::runtime
