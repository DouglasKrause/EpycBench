#include "results/prepared_pairwise_run_result.hpp"
#include "results/calculation_failure.hpp"

#include "epycbench/statistics/conclusion_classifier.hpp"
#include "epycbench/statistics/prepared_statistics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace epycbench::results::detail {
namespace {

using Failure = runtime::prepared_codec_v1::ActiveCodecFailure;
using FailureKind = runtime::prepared_codec_v1::ActivePipelineFailureKind;
using Stage = runtime::prepared_codec_v1::ActivePipelineStage;
using S = CalculationStage;
using ResourceError = runtime::prepared_resources_v1::PreparedResourceError;
using ResourceErrorCode = runtime::prepared_resources_v1::PreparedResourceErrorCode;

auto failure(FailureKind kind, std::uint64_t sequence = 0U,
             std::uint64_t limit = 0U, std::uint64_t observed = 0U) noexcept -> Failure {
    return {kind, Stage::active, sequence, limit, observed, std::nullopt};
}

auto size_value(std::uint64_t value, std::uint32_t index) noexcept
    -> std::expected<std::size_t, ResourceError> {
    if (value > std::numeric_limits<std::size_t>::max())
        return std::unexpected{ResourceError{ResourceErrorCode::size_unrepresentable,
                                             index, std::numeric_limits<std::size_t>::max(),
                                             value}};
    return static_cast<std::size_t>(value);
}

struct DiagnosticText final {
    std::string code;
    std::string stage;
    std::string field;
};

template <typename Value>
auto reserve_vector(std::vector<Value>& value, std::size_t capacity) -> void {
    value.reserve(capacity);
}

template <statistics::PrimaryMetric Metric> struct FamilyStorage final {
    std::vector<Metric> first;
    std::vector<Metric> second;
    std::vector<statistics::OutlierFlagState> first_flags;
    std::vector<statistics::OutlierFlagState> second_flags;
};

auto exact_bootstrap(const statistics::BlockBootstrapResult& left,
                     const statistics::BlockBootstrapResult& right) noexcept -> bool {
    return left.policy() == right.policy() &&
           left.log_effect_interval() == right.log_effect_interval() &&
           left.oriented_geometric_ratio_interval() ==
               right.oriented_geometric_ratio_interval();
}

} // namespace

struct PreparedPairwiseRunResultProjector::Storage final {
    std::optional<runtime::prepared_codec_v1::PreparedCanonicalWriter> writer;
    std::optional<runtime::prepared_codec_v1::PreparedFinalizedEvidenceReconstruction> reconstruction;
    FamilyStorage<statistics::Bandwidth> bandwidth;
    FamilyStorage<statistics::DependentLoadLatency> latency;
    std::vector<core::ObservationId> first_ids;
    std::vector<core::ObservationId> second_ids;
    std::vector<kernel::DependentLoadLatencyBlockId> block_ids;
    std::vector<statistics::PairedLogEffect> effects;
    std::vector<statistics::PairedLogEffect> resample;
    std::vector<statistics::PairedLogEffect> stopping_prefix;
    std::vector<double> metric_scratch;
    std::vector<double> primary;
    std::vector<double> secondary;
    std::vector<ControllerStoppingReplay> replays;
    std::vector<RecalculationMismatchDiagnostic> diagnostics;
    std::vector<DiagnosticText> diagnostic_text;
    bool consumed{};

};

class PreparedPairwiseRunResultAccess final {
  public:
    [[nodiscard]] static auto make(runtime::detail::FinalizedEvidenceComponents components,
                                   PairwiseResultValidity validity,
                                   std::optional<PairwiseStatistics> statistics,
                                   std::vector<ControllerStoppingReplay> stopping,
                                   std::vector<RecalculationMismatchDiagnostic> diagnostics,
                                   std::optional<FinalCalculationFailure> final_failure) noexcept
        -> PairwiseRunResult {
        return PairwiseRunResult{std::move(components.provenance),
                                 std::move(components.statistics_policy),
                                 std::move(components.monitoring_policy),
                                 std::move(components.repetition_policy),
                                 std::move(components.comparison_order),
                                 std::move(components.calibration),
                                 std::move(components.warmup),
                                 std::move(components.terminal),
                                 std::move(components.finalization),
                                 std::move(components.blocks),
                                 validity,
                                 std::move(statistics),
                                 std::move(stopping),
                                 std::move(diagnostics),
                                 final_failure};
    }
};

PreparedPairwiseRunResultProjector::PreparedPairwiseRunResultProjector(
    std::unique_ptr<Storage> storage) noexcept : storage_{std::move(storage)} {}
PreparedPairwiseRunResultProjector::PreparedPairwiseRunResultProjector(
    PreparedPairwiseRunResultProjector&&) noexcept = default;
PreparedPairwiseRunResultProjector::~PreparedPairwiseRunResultProjector() noexcept = default;

auto PreparedPairwiseRunResultProjector::prepare(
    const runtime::prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedPairwiseRunResultProjector, ResourceError> {
    return prepare_impl(resource,true);
}
auto PreparedPairwiseRunResultProjector::prepare_owned(
    const runtime::prepared_resources_v1::PreparedResourceValue& resource) noexcept
    -> std::expected<PreparedPairwiseRunResultProjector, ResourceError> {
    return prepare_impl(resource,false);
}
auto PreparedPairwiseRunResultProjector::prepare_impl(
    const runtime::prepared_resources_v1::PreparedResourceValue& resource,bool clone) noexcept
    -> std::expected<PreparedPairwiseRunResultProjector, ResourceError> {
    try {
        constexpr auto text_bytes =
            DiagnosticTextLimits::code + DiagnosticTextLimits::stage + DiagnosticTextLimits::field;
        if (resource.stocks.results_mismatch_diagnostics >
            (std::numeric_limits<std::size_t>::max() - 1U) / text_bytes)
            return std::unexpected{ResourceError{ResourceErrorCode::size_unrepresentable}};
        auto storage = std::make_unique<Storage>();
        if(clone) {
            auto writer_owner = runtime::prepared_resources_v1::PreparedResourceOwner::create(resource);
            if (!writer_owner) return std::unexpected{writer_owner.error()};
            auto writer = runtime::prepared_codec_v1::PreparedCanonicalWriter::prepare(
                std::move(*writer_owner));
            if (!writer) return std::unexpected{writer.error()};
            storage->writer.emplace(std::move(*writer));
            auto reconstruction =
                runtime::prepared_codec_v1::PreparedFinalizedEvidenceReconstruction::prepare(resource);
            if (!reconstruction) return std::unexpected{reconstruction.error()};
            storage->reconstruction.emplace(std::move(*reconstruction));
        }
        const auto first = size_value(resource.stocks.results_first_metrics, 0U);
        const auto second = size_value(resource.stocks.results_second_metrics, 1U);
        const auto first_ids = size_value(resource.stocks.results_first_observation_ids, 2U);
        const auto second_ids = size_value(resource.stocks.results_second_observation_ids, 3U);
        const auto blocks = size_value(resource.stocks.results_block_ids, 4U);
        const auto effects = size_value(resource.stocks.results_paired_effects, 5U);
        const auto first_flags = size_value(resource.stocks.results_first_outlier_states, 6U);
        const auto second_flags = size_value(resource.stocks.results_second_outlier_states, 7U);
        const auto primary = size_value(resource.stocks.statistics_double_scratch_primary, 8U);
        const auto secondary = size_value(resource.stocks.statistics_double_scratch_secondary, 9U);
        const auto resample = size_value(resource.stocks.statistics_effect_resample, 10U);
        const auto replays = size_value(resource.stocks.results_stopping_replays, 11U);
        const auto diagnostics = size_value(resource.stocks.results_mismatch_diagnostics, 12U);
        const auto metric_scratch = size_value(resource.stocks.statistics_metric_scratch, 13U);
        const auto stopping_prefix = size_value(resource.stocks.statistics_stopping_prefix, 14U);
        if (!first || !second || !first_ids || !second_ids || !blocks || !effects ||
            !first_flags || !second_flags || !primary || !secondary || !resample ||
            !replays || !diagnostics || !metric_scratch || !stopping_prefix)
            return std::unexpected{ResourceError{ResourceErrorCode::size_unrepresentable}};
        for (auto* family : {&storage->bandwidth.first, &storage->bandwidth.second})
            reserve_vector(*family, family == &storage->bandwidth.first ? *first : *second);
        for (auto* family : {&storage->latency.first, &storage->latency.second})
            reserve_vector(*family, family == &storage->latency.first ? *first : *second);
        reserve_vector(storage->bandwidth.first_flags, *first_flags);
        reserve_vector(storage->bandwidth.second_flags, *second_flags);
        reserve_vector(storage->latency.first_flags, *first_flags);
        reserve_vector(storage->latency.second_flags, *second_flags);
        reserve_vector(storage->first_ids, *first_ids);
        reserve_vector(storage->second_ids, *second_ids);
        reserve_vector(storage->block_ids, *blocks);
        reserve_vector(storage->effects, *effects);
        storage->resample.resize(*resample, *statistics::PairedLogEffect::create(0.0));
        storage->stopping_prefix.resize(*stopping_prefix,
                                        *statistics::PairedLogEffect::create(0.0));
        storage->metric_scratch.resize(*metric_scratch);
        storage->primary.resize(*primary);
        storage->secondary.resize(*secondary);
        reserve_vector(storage->replays, *replays);
        reserve_vector(storage->diagnostics, *diagnostics);
        storage->diagnostic_text.reserve(*diagnostics);
        for (std::size_t index{}; index < *diagnostics; ++index) {
            DiagnosticText text;
            text.code.reserve(DiagnosticTextLimits::code);
            text.stage.reserve(DiagnosticTextLimits::stage);
            text.field.reserve(DiagnosticTextLimits::field);
            storage->diagnostic_text.push_back(std::move(text));
        }
        return PreparedPairwiseRunResultProjector{std::move(storage)};
    } catch (...) {
        return std::unexpected{ResourceError{ResourceErrorCode::allocation_failure}};
    }
}

template <typename Observation, statistics::PrimaryMetric Metric, typename MakeMetric,
          typename Representatives, typename Effect, typename Summarize, typename Flag>
auto calculate_prepared(std::span<const runtime::PairwiseBlockAttemptEvidence> blocks,
                        const runtime::StatisticsProductionPolicy& policy, MakeMetric make_metric,
                        Representatives representatives, Effect effect, Summarize summarize,
                        Flag flag, FamilyStorage<Metric>& family,
                        std::vector<core::ObservationId>& first_ids,
                        std::vector<core::ObservationId>& second_ids,
                        std::vector<kernel::DependentLoadLatencyBlockId>& block_ids,
                        std::vector<statistics::PairedLogEffect>& effects,
                        std::span<double> primary, std::span<double> secondary,
                        std::span<statistics::PairedLogEffect> resample) noexcept
    -> std::expected<AuthoritativePairwiseStatistics<Metric>, CalculationError> {
    if (!valid_population(blocks))
        return std::unexpected{
            calculation_error(S::primary_metric, CalculationFailureKind::structural)};
    const auto count =
        static_cast<std::size_t>(std::ranges::count_if(blocks, [](const auto& block) {
            return block.completion == runtime::BlockAttemptCompletion::complete_valid;
        }));
    if (count > std::numeric_limits<std::size_t>::max() / 2U)
        return std::unexpected{
            calculation_error(S::primary_metric, CalculationFailureKind::capacity)};
    const auto observations = count * 2U;
    if (family.first.capacity() < observations || family.second.capacity() < observations ||
        first_ids.capacity() < observations || second_ids.capacity() < observations ||
        family.first_flags.capacity() < observations ||
        family.second_flags.capacity() < observations || block_ids.capacity() < count ||
        effects.capacity() < count || resample.size() < count || primary.size() < observations ||
        secondary.size() < observations ||
        primary.size() < policy.comparison_bootstrap_policy().replicate_count().value())
        return std::unexpected{
            calculation_error(S::primary_metric, CalculationFailureKind::capacity)};
    family.first.clear(); family.second.clear(); first_ids.clear(); second_ids.clear();
    block_ids.clear(); effects.clear();
    for (const auto& block : blocks) {
        if (block.completion != runtime::BlockAttemptCompletion::complete_valid) continue;
        std::array<std::optional<Metric>, 2> first{};
        std::array<std::optional<Metric>, 2> second{};
        std::size_t first_count{}, second_count{};
        for (const auto& slot : block.observations) {
            const auto* observation = std::get_if<Observation>(&slot.observation);
            if (!observation)
                return std::unexpected{
                    calculation_error(S::primary_metric, CalculationFailureKind::structural)};
            const auto metric = calculate_at(S::primary_metric, std::nullopt,
                                             [&] { return make_metric(*observation); });
            if (!metric)
                return std::unexpected{metric.error()};
            if (slot.slot.comparison_variant() == measurement::ComparisonVariantSlot::first) {
                if (first_count >= 2U || family.first.size() == family.first.capacity() ||
                    first_ids.size() == first_ids.capacity())
                    return std::unexpected{
                        calculation_error(S::primary_metric, CalculationFailureKind::capacity)};
                first[first_count++] = *metric; family.first.push_back(*metric);
                first_ids.push_back(slot.observation_id);
            } else {
                if (second_count >= 2U || family.second.size() == family.second.capacity() ||
                    second_ids.size() == second_ids.capacity())
                    return std::unexpected{
                        calculation_error(S::primary_metric, CalculationFailureKind::capacity)};
                second[second_count++] = *metric; family.second.push_back(*metric);
                second_ids.push_back(slot.observation_id);
            }
        }
        if (first_count != 2U || second_count != 2U ||
            block_ids.size() == block_ids.capacity() || effects.size() == effects.capacity())
            return std::unexpected{
                calculation_error(S::primary_metric, CalculationFailureKind::structural)};
        const auto reps = calculate_at(S::block_representatives, std::nullopt, [&] {
            return representatives(std::array<Metric, 2>{*first[0], *first[1]},
                                   std::array<Metric, 2>{*second[0], *second[1]});
        });
        if (!reps)
            return std::unexpected{reps.error()};
        const auto paired =
            calculate_at(S::paired_effect, std::nullopt, [&] { return effect(*reps); });
        if (!paired)
            return std::unexpected{paired.error()};
        block_ids.push_back(block.plan.block_id()); effects.push_back(*paired);
    }
    if (effects.empty())
        return std::unexpected{
            calculation_error(S::primary_metric, CalculationFailureKind::structural)};
    const auto first_summary = calculate_at(S::first_summary, std::nullopt, [&] {
        return summarize(family.first, primary, secondary);
    });
    if (!first_summary)
        return std::unexpected{first_summary.error()};
    const auto second_summary = calculate_at(S::second_summary, std::nullopt, [&] {
        return summarize(family.second, primary, secondary);
    });
    if (!second_summary)
        return std::unexpected{second_summary.error()};
    auto first_flags = calculate_at(S::first_outliers, std::nullopt, [&] {
        return flag(family.first, primary, secondary, family.first_flags);
    });
    if (!first_flags)
        return std::unexpected{first_flags.error()};
    auto second_flags = calculate_at(S::second_outliers, std::nullopt, [&] {
        return flag(family.second, primary, secondary, family.second_flags);
    });
    if (!second_flags)
        return std::unexpected{second_flags.error()};
    const auto comparison = calculate_at(S::comparison, std::nullopt, [&] {
        return statistics::aggregate_log_effects_prepared(effects, primary);
    });
    if (!comparison)
        return std::unexpected{comparison.error()};
    const auto interval = calculate_at(S::comparison_bootstrap, std::nullopt, [&] {
        return statistics::bootstrap_log_effects_prepared(
            effects, policy.comparison_bootstrap_policy(), {resample, primary, secondary});
    });
    if (!interval)
        return std::unexpected{interval.error()};
    const auto conclusion = statistics::classify_conclusion(
        comparison->mean_log_effect(), interval->log_effect_interval(),
        policy.practical_effect_threshold());
    AuthoritativePairwiseStatistics<Metric> result{
        {std::move(first_ids), *first_summary, std::move(*first_flags)},
        {std::move(second_ids), *second_summary, std::move(*second_flags)},
        std::move(block_ids), std::move(effects), *comparison, *interval, conclusion};
    return std::move(result);
}

auto PreparedPairwiseRunResultProjector::project(
    const runtime::FinalizedPairwiseRunEvidence& evidence,
    std::span<const core::LinuxCpuId> authenticated_cpus) noexcept
    -> std::expected<PairwiseRunResult, Failure> {
    if (!storage_ || storage_->consumed)
        return std::unexpected{failure(FailureKind::transport_state)};
    storage_->consumed = true;
    if(!storage_->writer || !storage_->reconstruction)
        return std::unexpected{failure(FailureKind::transport_state)};
    if (auto active = storage_->writer->activate(); !active)
        return std::unexpected{active.error()};
    if (auto written = runtime::prepared_codec_v1::write_finalized_evidence(
            *storage_->writer, evidence); !written)
        return std::unexpected{written.error()};
    auto body = storage_->writer->finish();
    if (!body) return std::unexpected{body.error()};
    auto clone = storage_->reconstruction->build(*body, authenticated_cpus);
    if (!clone) return std::unexpected{clone.error()};
    return project_components(runtime::detail::FinalizedEvidencePreparedAccess::take(std::move(*clone)));
}

auto PreparedPairwiseRunResultProjector::project_owned(
    runtime::FinalizedPairwiseRunEvidence&& evidence) noexcept
    -> std::expected<PairwiseRunResult, Failure> {
    if(!storage_ || storage_->consumed)
        return std::unexpected{failure(FailureKind::transport_state)};
    storage_->consumed=true;
    if(storage_->writer || storage_->reconstruction)
        return std::unexpected{failure(FailureKind::transport_state)};
    return project_components(runtime::detail::FinalizedEvidencePreparedAccess::take(std::move(evidence)));
}

auto PreparedPairwiseRunResultProjector::project_components(
    runtime::detail::FinalizedEvidenceComponents components) noexcept
    -> std::expected<PairwiseRunResult, Failure> {

    std::optional<PairwiseStatistics> final_statistics;
    std::optional<FinalCalculationFailure> final_failure;
    PairwiseResultValidity validity = PairwiseResultValidity::not_evaluated;
    const bool normal =
        std::holds_alternative<runtime::NormalRunTerminalEvidence>(components.terminal);
    const auto complete = std::ranges::find_if(components.blocks, [](const auto& block) {
        return block.completion == runtime::BlockAttemptCompletion::complete_valid;
    });
    if (normal && complete != components.blocks.end()) {
        if (std::holds_alternative<measurement::SequentialReadBandwidthObservation>(
                complete->observations.front().observation)) {
            auto value = calculate_prepared<measurement::SequentialReadBandwidthObservation,
                                            statistics::Bandwidth>(
                components.blocks, components.statistics_policy,
                [](const auto& observation) { return statistics::make_bandwidth_prepared(
                    observation.aggregate_work(), observation.enclosing_interval().elapsed()); },
                statistics::block_bandwidth_representatives_prepared,
                statistics::bandwidth_log_effect_prepared,
                [](const auto& values, auto primary, auto secondary) {
                    return statistics::summarize_bandwidth_prepared(values, {primary, secondary});
                },
                [](const auto& values, auto primary, auto secondary, auto& flags) {
                    return statistics::flag_bandwidth_outliers_prepared(
                        values, {primary, secondary, &flags});
                }, storage_->bandwidth, storage_->first_ids, storage_->second_ids,
                storage_->block_ids, storage_->effects, storage_->primary,
                storage_->secondary, storage_->resample);
            if (!value) {
                if (value.error().kind != CalculationFailureKind::numerical)
                    return std::unexpected{
                        failure(value.error().kind == CalculationFailureKind::capacity
                                    ? FailureKind::capacity_invariant
                                    : FailureKind::results_projection)};
                final_failure = value.error().fact;
            } else
                final_statistics.emplace(std::move(*value));
        } else {
            auto value = calculate_prepared<measurement::DependentLoadLatencyObservation,
                                            statistics::DependentLoadLatency>(
                components.blocks, components.statistics_policy,
                [](const auto& observation) {
                    return statistics::make_dependent_load_latency_prepared(
                        observation.enclosing_interval().elapsed(),
                        observation.aggregate_work());
                }, statistics::block_dependent_load_latency_representatives_prepared,
                statistics::dependent_load_latency_log_effect_prepared,
                [](const auto& values, auto primary, auto secondary) {
                    return statistics::summarize_dependent_load_latency_prepared(
                        values, {primary, secondary});
                },
                [](const auto& values, auto primary, auto secondary, auto& flags) {
                    return statistics::flag_dependent_load_latency_outliers_prepared(
                        values, {primary, secondary, &flags});
                }, storage_->latency, storage_->first_ids, storage_->second_ids,
                storage_->block_ids, storage_->effects, storage_->primary,
                storage_->secondary, storage_->resample);
            if (!value) {
                if (value.error().kind != CalculationFailureKind::numerical)
                    return std::unexpected{
                        failure(value.error().kind == CalculationFailureKind::capacity
                                    ? FailureKind::capacity_invariant
                                    : FailureKind::results_projection)};
                final_failure = value.error().fact;
            } else
                final_statistics.emplace(std::move(*value));
        }
        validity = final_failure ? PairwiseResultValidity::invalid : PairwiseResultValidity::valid;
    }

    // Reserve the complete historical output and workspace before any numerical seam.
    if (components.stopping.size() > storage_->replays.capacity() ||
        components.stopping.size() > storage_->diagnostics.capacity())
        return std::unexpected{failure(FailureKind::capacity_invariant)};
    for (const auto& runner : components.stopping) {
        if (!valid_population(components.blocks, runner.boundary_block_id.value()))
            return std::unexpected{failure(FailureKind::results_projection)};
        const auto prefix_count = static_cast<std::size_t>(
            std::ranges::count_if(components.blocks, [&](const auto& block) {
                return block.plan.block_id().value() <= runner.boundary_block_id.value() &&
                       block.completion == runtime::BlockAttemptCompletion::complete_valid;
            }));
        if (storage_->stopping_prefix.size() < prefix_count ||
            storage_->resample.size() < prefix_count || storage_->primary.size() < prefix_count ||
            storage_->secondary.size() < prefix_count ||
            storage_->primary.size() <
                components.statistics_policy.stopping_bootstrap_policy().replicate_count().value())
            return std::unexpected{failure(FailureKind::capacity_invariant)};
    }
    storage_->replays.clear(); storage_->diagnostics.clear();
    const auto add_diagnostic = [&](kernel::DependentLoadLatencyBlockId boundary,
                                    std::optional<FinalCalculationFailure> fact) -> bool {
        const auto index = storage_->diagnostics.size();
        if (index >= storage_->diagnostic_text.size() || index == storage_->diagnostics.capacity())
            return false;
        auto& text = storage_->diagnostic_text[index];
        const auto code =
            fact ? fact->stable_code() : std::string_view{"results.recalculation_mismatch"};
        const auto stage = fact ? fact->stage_name() : std::string_view{"stopping_replay"};
        const auto field = fact ? std::string_view{"calculation_unavailable"}
                                : std::string_view{"exact_typed_evidence"};
        if (!DiagnosticTextLimits::fits(code, stage, field))
            return false;
        if (code.size() > text.code.capacity() || stage.size() > text.stage.capacity() ||
            field.size() > text.field.capacity())
            return false;
        text.code.assign(code);
        text.stage.assign(stage);
        text.field.assign(field);
        storage_->diagnostics.push_back({std::move(text.code), boundary, std::move(text.stage),
                                         std::move(text.field), std::nullopt, std::nullopt});
        return true;
    };
    for (const auto& runner : components.stopping) {
        const auto boundary = runner.boundary_block_id.value();
        const auto replayed = [&]() -> std::expected<ControllerStoppingReplay, CalculationError> {
            if (complete == components.blocks.end())
                return std::unexpected{
                    calculation_error(S::primary_metric, CalculationFailureKind::structural)};
            const auto count =
                std::holds_alternative<measurement::SequentialReadBandwidthObservation>(
                    complete->observations.front().observation)
                    ? rebuild_prefix<measurement::SequentialReadBandwidthObservation,
                                     statistics::Bandwidth>(
                          components.blocks, boundary, storage_->stopping_prefix,
                          [](const auto& value) {
                              return statistics::make_bandwidth_prepared(
                                  value.aggregate_work(), value.enclosing_interval().elapsed());
                          },
                          statistics::block_bandwidth_representatives_prepared,
                          statistics::bandwidth_log_effect_prepared)
                    : rebuild_prefix<measurement::DependentLoadLatencyObservation,
                                     statistics::DependentLoadLatency>(
                          components.blocks, boundary, storage_->stopping_prefix,
                          [](const auto& value) {
                              return statistics::make_dependent_load_latency_prepared(
                                  value.enclosing_interval().elapsed(), value.aggregate_work());
                          },
                          statistics::block_dependent_load_latency_representatives_prepared,
                          statistics::dependent_load_latency_log_effect_prepared);
            if (!count)
                return std::unexpected{count.error()};
            const auto prefix =
                std::span<const statistics::PairedLogEffect>{storage_->stopping_prefix}.first(
                    *count);
            const auto comparison = calculate_at(S::stopping_comparison, boundary, [&] {
                return statistics::aggregate_log_effects_prepared(prefix, storage_->primary);
            });
            if (!comparison)
                return std::unexpected{comparison.error()};
            const auto bootstrap = calculate_at(S::stopping_bootstrap, boundary, [&] {
                return statistics::bootstrap_log_effects_prepared(
                    prefix, components.statistics_policy.stopping_bootstrap_policy(),
                    {storage_->resample, storage_->primary, storage_->secondary});
            });
            if (!bootstrap)
                return std::unexpected{bootstrap.error()};
            const auto evaluation = calculate_at(S::stopping_evaluation, boundary, [&] {
                return statistics::evaluate_adaptive_stopping_prepared(
                    comparison->oriented_geometric_ratio(),
                    bootstrap->oriented_geometric_ratio_interval(),
                    components.statistics_policy.relative_half_width_target());
            });
            if (!evaluation)
                return std::unexpected{evaluation.error()};
            const auto decision =
                evaluation->attainment() == statistics::PrecisionTargetAttainment::target_met
                    ? measurement::PrecisionStoppingDecision::target_met
                    : measurement::PrecisionStoppingDecision::target_not_met;
            const bool agreement = exact_bootstrap(*bootstrap, runner.bootstrap) &&
                                   *evaluation == runner.evaluation && decision == runner.decision;
            return ControllerStoppingReplay{runner.boundary_block_id, *bootstrap, *evaluation,
                                            decision, agreement};
        }();
        if (!replayed) {
            if (replayed.error().kind != CalculationFailureKind::numerical)
                return std::unexpected{
                    failure(replayed.error().kind == CalculationFailureKind::capacity
                                ? FailureKind::capacity_invariant
                                : FailureKind::results_projection)};
            if (!add_diagnostic(runner.boundary_block_id, replayed.error().fact))
                return std::unexpected{failure(FailureKind::capacity_invariant)};
            validity = PairwiseResultValidity::invalid;
            continue;
        }
        if (storage_->replays.size() == storage_->replays.capacity())
            return std::unexpected{failure(FailureKind::capacity_invariant)};
        storage_->replays.push_back(*replayed);
        if (!replayed->exact_agreement) {
            validity = PairwiseResultValidity::invalid;
            if (!add_diagnostic(runner.boundary_block_id, std::nullopt))
                return std::unexpected{failure(FailureKind::capacity_invariant)};
        }
    }
    return PreparedPairwiseRunResultAccess::make(
        std::move(components), validity, std::move(final_statistics), std::move(storage_->replays),
        std::move(storage_->diagnostics), final_failure);
}

} // namespace epycbench::results::detail
