#include "epycbench/results/pairwise_run_result.hpp"
#include "results/pairwise_statistics_calculation.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace epycbench::results {
namespace {
using detail::calculate_at;
using detail::CalculationError;
using detail::CalculationFailureKind;
using S = CalculationStage;
auto projection_error(std::string reason, const core::Error* cause = nullptr) -> core::Error {
    nlohmann::json context{{"reason", std::move(reason)}};
    if (cause) context["underlying_code"] = cause->code.value();
    return {.category = core::ErrorCategory::results,
            .code = core::StableErrorCode{"results.projection.invalid_evidence"},
            .message = "Pairwise result projection rejected finalized evidence",
            .operation = "project_pairwise_run_result",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

template <typename Observation, typename Metric, typename MakeMetric, typename Representatives,
          typename Effect, typename Summarize, typename Flag>
auto calculate(std::span<const runtime::PairwiseBlockAttemptEvidence> blocks,
               const runtime::StatisticsProductionPolicy& policy, MakeMetric make_metric,
               Representatives representatives, Effect effect, Summarize summarize, Flag flag)
    -> std::expected<AuthoritativePairwiseStatistics<Metric>, CalculationError> {
    if (!detail::valid_population(blocks))
        return std::unexpected{
            detail::calculation_error(S::primary_metric, CalculationFailureKind::structural)};
    std::vector<Metric> first;
    std::vector<Metric> second;
    std::vector<core::ObservationId> first_ids;
    std::vector<core::ObservationId> second_ids;
    std::vector<kernel::DependentLoadLatencyBlockId> block_ids;
    std::vector<statistics::PairedLogEffect> effects;
    const auto count = static_cast<std::size_t>(std::ranges::count_if(blocks, [](const auto& block) {
        return block.completion == runtime::BlockAttemptCompletion::complete_valid;
    }));
    if (count > std::numeric_limits<std::size_t>::max() / 2U)
        return std::unexpected{detail::calculation_error(S::primary_metric, CalculationFailureKind::capacity)};
    first.reserve(count * 2U);
    second.reserve(count * 2U);
    first_ids.reserve(count * 2U);
    second_ids.reserve(count * 2U);
    block_ids.reserve(count);
    effects.reserve(count);
    for (const auto& block : blocks) {
        if (block.completion != runtime::BlockAttemptCompletion::complete_valid) continue;
        std::array<std::optional<Metric>, 2> block_first{};
        std::array<std::optional<Metric>, 2> block_second{};
        std::size_t fi{};
        std::size_t si{};
        for (const auto& slot : block.observations) {
            const auto* observation = std::get_if<Observation>(&slot.observation);
            if (!observation)
                return std::unexpected{detail::calculation_error(
                    S::primary_metric, CalculationFailureKind::structural)};
            auto metric = calculate_at(S::primary_metric, std::nullopt,
                                       [&] { return make_metric(*observation); });
            if (!metric)
                return std::unexpected{metric.error()};
            if (slot.slot.comparison_variant() == measurement::ComparisonVariantSlot::first) {
                if (fi >= 2)
                    return std::unexpected{detail::calculation_error(
                        S::primary_metric, CalculationFailureKind::structural)};
                block_first[fi++] = *metric;
                first.push_back(*metric);
                first_ids.push_back(slot.observation_id);
            } else {
                if (si >= 2)
                    return std::unexpected{detail::calculation_error(
                        S::primary_metric, CalculationFailureKind::structural)};
                block_second[si++] = *metric;
                second.push_back(*metric);
                second_ids.push_back(slot.observation_id);
            }
        }
        if (fi != 2 || si != 2)
            return std::unexpected{
                detail::calculation_error(S::primary_metric, CalculationFailureKind::structural)};
        const std::array<Metric, 2> first_values{*block_first[0], *block_first[1]};
        const std::array<Metric, 2> second_values{*block_second[0], *block_second[1]};
        auto reps = calculate_at(S::block_representatives, std::nullopt,
                                 [&] { return representatives(first_values, second_values); });
        if (!reps)
            return std::unexpected{reps.error()};
        auto paired = calculate_at(S::paired_effect, std::nullopt, [&] { return effect(*reps); });
        if (!paired)
            return std::unexpected{paired.error()};
        block_ids.push_back(block.plan.block_id());
        effects.push_back(*paired);
    }
    if (effects.empty())
        return std::unexpected{
            detail::calculation_error(S::primary_metric, CalculationFailureKind::structural)};
    auto first_summary =
        calculate_at(S::first_summary, std::nullopt, [&] { return summarize(first); });
    if (!first_summary)
        return std::unexpected{first_summary.error()};
    auto second_summary =
        calculate_at(S::second_summary, std::nullopt, [&] { return summarize(second); });
    if (!second_summary)
        return std::unexpected{second_summary.error()};
    auto first_flags = calculate_at(S::first_outliers, std::nullopt, [&] { return flag(first); });
    if (!first_flags)
        return std::unexpected{first_flags.error()};
    auto second_flags =
        calculate_at(S::second_outliers, std::nullopt, [&] { return flag(second); });
    if (!second_flags)
        return std::unexpected{second_flags.error()};
    auto comparison = calculate_at(S::comparison, std::nullopt,
                                   [&] { return statistics::aggregate_log_effects(effects); });
    if (!comparison)
        return std::unexpected{comparison.error()};
    auto interval = calculate_at(S::comparison_bootstrap, std::nullopt, [&] {
        return statistics::bootstrap_log_effects(effects, policy.comparison_bootstrap_policy());
    });
    if (!interval)
        return std::unexpected{interval.error()};
    const auto conclusion = statistics::classify_conclusion(
        comparison->mean_log_effect(), interval->log_effect_interval(),
        policy.practical_effect_threshold());
    return AuthoritativePairwiseStatistics<Metric>{
        {std::move(first_ids), *first_summary, std::move(*first_flags)},
        {std::move(second_ids), *second_summary, std::move(*second_flags)},
        std::move(block_ids), std::move(effects), *comparison, *interval, conclusion};
}

auto exact_bootstrap(const statistics::BlockBootstrapResult& left,
                     const statistics::BlockBootstrapResult& right) -> bool {
    return left.policy() == right.policy() &&
           left.log_effect_interval() == right.log_effect_interval() &&
           left.oriented_geometric_ratio_interval() ==
               right.oriented_geometric_ratio_interval();
}


auto check_outcome(const measurement::ObservationValidityEvidence& validity,
                   measurement::ObservationValidityCheckId id)
    -> std::optional<measurement::ObservationValidityCheckOutcome> {
    for (const auto& check : validity.checks())
        if (check.id() == id) return check.outcome();
    return std::nullopt;
}

auto placement_outcome(std::span<const measurement::WorkerPlacementValidityEvidence> values)
    -> measurement::ObservationValidityCheckOutcome {
    using State = measurement::WorkerPlacementValidityState;
    using Outcome = measurement::ObservationValidityCheckOutcome;
    const auto has = [&](State state) { return std::ranges::any_of(values, [=](const auto& value) {
        return value.state() == state;
    }); };
    if (has(State::not_qualified)) return Outcome::fail;
    if (has(State::unavailable)) return Outcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& value) {
            return value.state() == State::not_applicable;
        })) return Outcome::not_applicable;
    return Outcome::pass;
}

auto migration_outcome(std::span<const measurement::WorkerMigrationValidityEvidence> values)
    -> measurement::ObservationValidityCheckOutcome {
    using State = measurement::WorkerMigrationValidityState;
    using Outcome = measurement::ObservationValidityCheckOutcome;
    const auto has = [&](State state) { return std::ranges::any_of(values, [=](const auto& value) {
        return value.state() == state;
    }); };
    if (has(State::migrated)) return Outcome::fail;
    if (has(State::unavailable)) return Outcome::unavailable;
    if (std::ranges::all_of(values, [](const auto& value) {
            return value.state() == State::not_applicable;
        })) return Outcome::not_applicable;
    return Outcome::pass;
}

auto validate_projection(const runtime::FinalizedPairwiseRunEvidence& evidence) -> bool {
    const bool normal = std::holds_alternative<runtime::NormalRunTerminalEvidence>(evidence.terminal());
    if (!evidence.finalization()) {
        if (normal) return false;
        return !evidence.calibration() && !evidence.warmup() && evidence.blocks().empty() &&
               evidence.stopping_evaluations().empty();
    }
    const auto& final = *evidence.finalization();
    if (final.first.slot != measurement::ComparisonVariantSlot::first ||
        final.second.slot != measurement::ComparisonVariantSlot::second ||
        final.first.workers.empty() ||
        final.first.workers.size() != final.second.workers.size()) return false;
    std::array<std::span<const runtime::FinalizedWorkerEvidence>, 2> admitted{
        final.first.workers, final.second.workers};
    for (std::size_t index = 0; index < admitted[0].size(); ++index) {
        if (admitted[0][index].worker_id != admitted[1][index].worker_id) return false;
        if (index != 0U &&
            admitted[0][index - 1U].worker_id.value() >= admitted[0][index].worker_id.value())
            return false;
    }
    for (const auto& block : evidence.blocks()) {
        for (const auto& factual : block.observations) {
            const auto variant = factual.slot.comparison_variant() ==
                                         measurement::ComparisonVariantSlot::first ? 0U : 1U;
            const bool associated = std::visit([&](const auto& observation) {
                const auto workers = observation.workers();
                if (workers.size() != admitted[variant].size() ||
                    workers.size() != factual.placements.size() ||
                    workers.size() != factual.migrations.size()) return false;
                for (std::size_t index = 0; index < workers.size(); ++index) {
                    if (workers[index].worker_id() != admitted[variant][index].worker_id ||
                        workers[index].worker_id() != factual.placements[index].worker_id() ||
                        workers[index].worker_id() != factual.migrations[index].worker_id())
                        return false;
                }
                return true;
            }, factual.observation);
            if (!associated ||
                check_outcome(factual.validity, measurement::ObservationValidityCheckId::placement) !=
                    placement_outcome(factual.placements) ||
                check_outcome(factual.validity, measurement::ObservationValidityCheckId::migration) !=
                    migration_outcome(factual.migrations)) return false;
        }
    }
    const auto error_count = [&]() -> std::size_t {
        if (const auto* operational =
                std::get_if<runtime::OperationalRunTerminalEvidence>(&evidence.terminal()))
            return 1U + operational->secondary_errors.size();
        return 0U;
    }();
    std::vector<std::uint64_t> ordinals;
    const auto validate_variant = [&](const runtime::FinalizedVariantEvidence& variant,
                                      std::size_t index) {
        for (std::size_t worker_index = 0; worker_index < variant.workers.size(); ++worker_index) {
            const auto& worker = variant.workers[worker_index];
            if (worker.worker_id != admitted[index][worker_index].worker_id) return false;
            for (const auto ordinal : {worker.primary_error_ordinal,
                                       worker.cleanup_error_ordinal}) {
                if (ordinal) {
                    if (normal || *ordinal >= error_count ||
                        std::ranges::find(ordinals, *ordinal) != ordinals.end()) return false;
                    ordinals.push_back(*ordinal);
                }
            }
            if (worker.primary_error_ordinal && worker.cleanup_error_ordinal &&
                *worker.primary_error_ordinal >= *worker.cleanup_error_ordinal) return false;
            const auto* available = worker.region.available();
            if (normal) {
                if (!available || available->request().worker_id() != worker.worker_id ||
                    available->disposition() != memory::RegionFinalizationDisposition::succeeded ||
                    !available->cleanup_evidence().release_attempted() ||
                    !available->cleanup_evidence().release_succeeded() ||
                    available->cleanup_evidence().mapping_active_after()) return false;
                if (available->request().verify_after()) {
                    if (!available->post_run() ||
                        available->post_run()->state() != memory::PostRunVerificationState::observed ||
                        available->post_run()->qualification_state() !=
                            memory::PlacementQualificationState::qualified) return false;
                } else if (!available->post_run() ||
                           available->post_run()->state() !=
                               memory::PostRunVerificationState::skipped) {
                    return false;
                }
            } else {
                if (available && available->request().worker_id() != worker.worker_id) return false;
                const bool succeeded = available &&
                    available->disposition() == memory::RegionFinalizationDisposition::succeeded;
                if (succeeded && (worker.primary_error_ordinal || worker.cleanup_error_ordinal))
                    return false;
                if (!succeeded && !worker.primary_error_ordinal) return false;
                if (worker.cleanup_error_ordinal && !worker.primary_error_ordinal) return false;
            }
            if (available) {
                const auto qualification = available->pre_run().qualification_state();
                for (const auto& block : evidence.blocks()) {
                    for (const auto& factual : block.observations) {
                        if ((factual.slot.comparison_variant() ==
                             measurement::ComparisonVariantSlot::first ? 0U : 1U) != index)
                            continue;
                        const auto state = factual.placements[worker_index].state();
                        if ((available->request().verify_before() &&
                             (!qualification || available->pre_run().state() !=
                                  memory::PreRunDiagnosticState::observed)) ||
                            (!available->request().verify_before() &&
                             (qualification || available->pre_run().state() !=
                                  memory::PreRunDiagnosticState::skipped)) ||
                            (qualification == memory::PlacementQualificationState::qualified &&
                             state != measurement::WorkerPlacementValidityState::qualified) ||
                            (qualification == memory::PlacementQualificationState::not_qualified &&
                             state != measurement::WorkerPlacementValidityState::not_qualified) ||
                            (!qualification &&
                             state != measurement::WorkerPlacementValidityState::not_applicable))
                            return false;
                    }
                }
            }
        }
        return true;
    };
    if (!validate_variant(final.first, 0U) || !validate_variant(final.second, 1U)) return false;
    if (!ordinals.empty()) {
        for (std::size_t index = 1; index < ordinals.size(); ++index)
            if (ordinals[index] != ordinals[index - 1U] + 1U) return false;
    }
    return true;
}
} // namespace

PairwiseRunResult::PairwiseRunResult(
    runtime::PairwiseRunProvenance provenance,
    runtime::StatisticsProductionPolicy statistics_policy,
    monitoring::MonitoringExecutionPolicy monitoring_policy,
    measurement::RepetitionPolicy repetition_policy,
    measurement::ComparisonOrderPolicy comparison_order,
    std::optional<runtime::PairwiseCalibrationEvidence> calibration,
    std::optional<runtime::PairwiseWarmupEvidence> warmup,
    runtime::PairwiseRunTerminalEvidence terminal,
    std::optional<runtime::PairwiseFinalizationEvidence> finalization,
    std::vector<runtime::PairwiseBlockAttemptEvidence> blocks, PairwiseResultValidity validity,
    std::optional<PairwiseStatistics> statistics, std::vector<ControllerStoppingReplay> stopping,
    std::vector<RecalculationMismatchDiagnostic> diagnostics,
    std::optional<FinalCalculationFailure> final_failure) noexcept
    : provenance_{std::move(provenance)}, statistics_policy_{std::move(statistics_policy)},
      monitoring_policy_{std::move(monitoring_policy)},
      repetition_policy_{std::move(repetition_policy)},
      comparison_order_{std::move(comparison_order)}, calibration_{std::move(calibration)},
      warmup_{std::move(warmup)}, terminal_{std::move(terminal)},
      finalization_{std::move(finalization)}, blocks_{std::move(blocks)}, validity_{validity},
      statistics_{std::move(statistics)}, stopping_{std::move(stopping)},
      diagnostics_{std::move(diagnostics)}, final_failure_{final_failure} {}
auto PairwiseRunResult::provenance() const noexcept -> const runtime::PairwiseRunProvenance& { return provenance_; }
auto PairwiseRunResult::statistics_policy() const noexcept -> const runtime::StatisticsProductionPolicy& { return statistics_policy_; }
auto PairwiseRunResult::monitoring_policy() const noexcept -> const monitoring::MonitoringExecutionPolicy& { return monitoring_policy_; }
auto PairwiseRunResult::repetition_policy() const noexcept -> const measurement::RepetitionPolicy& { return repetition_policy_; }
auto PairwiseRunResult::comparison_order() const noexcept -> const measurement::ComparisonOrderPolicy& { return comparison_order_; }
auto PairwiseRunResult::calibration() const noexcept -> const std::optional<runtime::PairwiseCalibrationEvidence>& { return calibration_; }
auto PairwiseRunResult::warmup() const noexcept -> const std::optional<runtime::PairwiseWarmupEvidence>& { return warmup_; }
auto PairwiseRunResult::terminal() const noexcept -> const runtime::PairwiseRunTerminalEvidence& { return terminal_; }
auto PairwiseRunResult::validity() const noexcept -> PairwiseResultValidity { return validity_; }
auto PairwiseRunResult::authoritative_statistics() const noexcept -> const std::optional<PairwiseStatistics>& { return statistics_; }
auto PairwiseRunResult::stopping_replays() const noexcept -> std::span<const ControllerStoppingReplay> { return stopping_; }
auto PairwiseRunResult::diagnostics() const noexcept -> std::span<const RecalculationMismatchDiagnostic> { return diagnostics_; }
auto PairwiseRunResult::raw_blocks() const noexcept -> std::span<const runtime::PairwiseBlockAttemptEvidence> { return blocks_; }
auto PairwiseRunResult::finalization() const noexcept
    -> const std::optional<runtime::PairwiseFinalizationEvidence>& { return finalization_; }

auto detail::calculate_pairwise_statistics(
    std::span<const runtime::PairwiseBlockAttemptEvidence> blocks,
    const runtime::StatisticsProductionPolicy& policy)
    -> std::expected<PairwiseStatistics, CalculationError> {
    if (!valid_population(blocks))
        return std::unexpected{calculation_error(S::primary_metric, CalculationFailureKind::structural)};
    const auto complete = std::ranges::find_if(blocks, [](const auto& block) {
        return block.completion == runtime::BlockAttemptCompletion::complete_valid;
    });
    if (std::holds_alternative<measurement::SequentialReadBandwidthObservation>(complete->observations.front().observation)) {
        auto value = calculate<measurement::SequentialReadBandwidthObservation, statistics::Bandwidth>(
            blocks, policy, [](const auto& observation) {
                return statistics::Bandwidth::create(observation.aggregate_work(), observation.enclosing_interval().elapsed());
            }, statistics::block_bandwidth_representatives, statistics::bandwidth_log_effect,
            statistics::summarize_bandwidth, statistics::flag_bandwidth_outliers);
        if (!value) return std::unexpected{value.error()};
        return PairwiseStatistics{std::move(*value)};
    }
    auto value = calculate<measurement::DependentLoadLatencyObservation, statistics::DependentLoadLatency>(
        blocks, policy, [](const auto& observation) {
            return statistics::DependentLoadLatency::create(observation.enclosing_interval().elapsed(), observation.aggregate_work());
        }, statistics::block_dependent_load_latency_representatives, statistics::dependent_load_latency_log_effect,
        statistics::summarize_dependent_load_latency, statistics::flag_dependent_load_latency_outliers);
    if (!value) return std::unexpected{value.error()};
    return PairwiseStatistics{std::move(*value)};
}

auto project_pairwise_run_result(const runtime::FinalizedPairwiseRunEvidence& evidence)
    -> std::expected<PairwiseRunResult, core::Error> {
    try {
        if (!validate_projection(evidence))
            return std::unexpected{projection_error("finalization_or_observation_association")};
        std::optional<PairwiseStatistics> final_statistics;
        std::optional<FinalCalculationFailure> final_failure;
        PairwiseResultValidity validity = PairwiseResultValidity::not_evaluated;
        const bool normal = std::holds_alternative<runtime::NormalRunTerminalEvidence>(evidence.terminal());
        const auto complete = std::ranges::find_if(evidence.blocks(), [](const auto& block) {
            return block.completion == runtime::BlockAttemptCompletion::complete_valid;
        });
        if (normal && complete != evidence.blocks().end()) {
            auto calculated = detail::calculate_pairwise_statistics(evidence.blocks(), evidence.statistics_policy());
            if (!calculated) {
                if (calculated.error().kind != CalculationFailureKind::numerical)
                    return std::unexpected{projection_error("calculation_structure")};
                final_failure = calculated.error().fact;
            } else {
                final_statistics.emplace(std::move(*calculated));
            }
            validity =
                final_failure ? PairwiseResultValidity::invalid : PairwiseResultValidity::valid;
        }

        std::vector<ControllerStoppingReplay> replays;
        std::vector<RecalculationMismatchDiagnostic> diagnostics;
        std::vector<statistics::PairedLogEffect> prefix(evidence.blocks().size(),
                                                        *statistics::PairedLogEffect::create(0.0));
        for (const auto& runner : evidence.stopping_evaluations()) {
            const auto boundary = runner.boundary_block_id.value();
            auto replay = [&]() -> std::expected<ControllerStoppingReplay, CalculationError> {
                if (complete == evidence.blocks().end())
                    return std::unexpected{detail::calculation_error(
                        S::primary_metric, CalculationFailureKind::structural)};
                auto count =
                    std::holds_alternative<measurement::SequentialReadBandwidthObservation>(
                        complete->observations.front().observation)
                        ? detail::rebuild_prefix<measurement::SequentialReadBandwidthObservation,
                                                 statistics::Bandwidth>(
                              evidence.blocks(), boundary, prefix,
                              [](const auto& value) {
                                  return statistics::Bandwidth::create(
                                      value.aggregate_work(), value.enclosing_interval().elapsed());
                              },
                              statistics::block_bandwidth_representatives,
                              statistics::bandwidth_log_effect)
                        : detail::rebuild_prefix<measurement::DependentLoadLatencyObservation,
                                                 statistics::DependentLoadLatency>(
                              evidence.blocks(), boundary, prefix,
                              [](const auto& value) {
                                  return statistics::DependentLoadLatency::create(
                                      value.enclosing_interval().elapsed(), value.aggregate_work());
                              },
                              statistics::block_dependent_load_latency_representatives,
                              statistics::dependent_load_latency_log_effect);
                if (!count)
                    return std::unexpected{count.error()};
                const auto effects =
                    std::span<const statistics::PairedLogEffect>{prefix}.first(*count);
                auto comparison = calculate_at(S::stopping_comparison, boundary, [&] {
                    return statistics::aggregate_log_effects(effects);
                });
                if (!comparison)
                    return std::unexpected{comparison.error()};
                auto bootstrap = calculate_at(S::stopping_bootstrap, boundary, [&] {
                    return statistics::bootstrap_log_effects(
                        effects, evidence.statistics_policy().stopping_bootstrap_policy());
                });
                if (!bootstrap)
                    return std::unexpected{bootstrap.error()};
                auto evaluation = calculate_at(S::stopping_evaluation, boundary, [&] {
                    return statistics::evaluate_adaptive_stopping(
                        comparison->oriented_geometric_ratio(),
                        bootstrap->oriented_geometric_ratio_interval(),
                        evidence.statistics_policy().relative_half_width_target());
                });
                if (!evaluation)
                    return std::unexpected{evaluation.error()};
                const auto decision =
                    evaluation->attainment() == statistics::PrecisionTargetAttainment::target_met
                        ? measurement::PrecisionStoppingDecision::target_met
                        : measurement::PrecisionStoppingDecision::target_not_met;
                const bool agreement = exact_bootstrap(*bootstrap, runner.bootstrap) &&
                                       *evaluation == runner.evaluation &&
                                       decision == runner.decision;
                return ControllerStoppingReplay{runner.boundary_block_id, *bootstrap, *evaluation,
                                                decision, agreement};
            }();
            if (!replay) {
                if (replay.error().kind != CalculationFailureKind::numerical)
                    return std::unexpected{projection_error("stopping_structure")};
                const auto fact = replay.error().fact;
                diagnostics.push_back({std::string{fact.stable_code()}, runner.boundary_block_id,
                                       std::string{fact.stage_name()}, "calculation_unavailable",
                                       std::nullopt, std::nullopt});
                validity = PairwiseResultValidity::invalid;
                continue;
            }
            replays.push_back(*replay);
            if (!replay->exact_agreement) {
                validity = PairwiseResultValidity::invalid;
                diagnostics.push_back({"results.recalculation_mismatch", runner.boundary_block_id,
                                       "stopping_replay", "exact_typed_evidence", std::nullopt,
                                       std::nullopt});
            }
        }
        std::vector<runtime::PairwiseBlockAttemptEvidence> raw_blocks;
        raw_blocks.reserve(evidence.blocks().size());
        for (const auto& block : evidence.blocks()) raw_blocks.push_back(block);
        return PairwiseRunResult{evidence.provenance(),
                                 evidence.statistics_policy(),
                                 evidence.monitoring_policy(),
                                 evidence.repetition_policy(),
                                 evidence.comparison_order(),
                                 evidence.calibration(),
                                 evidence.warmup(),
                                 evidence.terminal(),
                                 evidence.finalization(),
                                 std::move(raw_blocks),
                                 validity,
                                 std::move(final_statistics),
                                 std::move(replays),
                                 std::move(diagnostics),
                                 final_failure};
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::length_error&) {
        auto error = projection_error("resource_limit");
        error.code = core::StableErrorCode{"results.projection.resource_failure"};
        return std::unexpected{std::move(error)};
    } catch (...) {
        auto error = projection_error("internal_failure");
        error.code = core::StableErrorCode{"results.projection.internal_failure"};
        return std::unexpected{std::move(error)};
    }
}
} // namespace epycbench::results
