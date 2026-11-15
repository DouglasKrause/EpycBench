#pragma once
#include "results/calculation_failure_identity.hpp"
#include "epycbench/statistics/prepared_statistics.hpp"

namespace epycbench::results::detail {
struct DiagnosticTextLimits {
    static constexpr std::size_t code = 47U;
    static constexpr std::size_t stage = 21U;
    static constexpr std::size_t field = 23U;
    static constexpr auto fits(std::string_view c, std::string_view s, std::string_view f) noexcept
        -> bool {
        return c.size() <= code && s.size() <= stage && f.size() <= field;
    }
};
enum class CalculationFailureKind { structural, capacity, numerical };
struct CalculationError {
    CalculationFailureKind kind;
    FinalCalculationFailure fact;
};
constexpr auto calculation_error(CalculationStage stage, CalculationFailureKind kind) noexcept
    -> CalculationError {
    return {kind, {stage, calculation_identity(stage)}};
}
inline auto classify(CalculationStage stage, const core::Error& error) noexcept
    -> CalculationError {
    auto failure = calculation_error(stage, CalculationFailureKind::numerical);
    if (error.code.value() != failure.fact.stable_code())
        failure.kind = CalculationFailureKind::structural;
    return failure;
}
constexpr auto classify(CalculationStage stage,
                        statistics::PreparedStatisticsFailure failure) noexcept
    -> CalculationError {
    using F = statistics::PreparedStatisticsFailure;
    return calculation_error(
        stage, failure == F::numeric_failure          ? CalculationFailureKind::numerical
               : failure == F::insufficient_workspace ? CalculationFailureKind::capacity
                                                      : CalculationFailureKind::structural);
}
#ifdef EPYCBENCH_RESULTS_CALCULATION_TESTING
namespace calculation_test {
struct Rule {
    bool enabled{};
    CalculationStage stage{};
    bool historical{};
    std::optional<std::uint64_t> boundary;
    CalculationFailureKind kind{CalculationFailureKind::numerical};
};
inline thread_local std::array<Rule, 16> rules{};
inline void reset() noexcept { rules = {}; }
} // namespace calculation_test
#endif
// Production has no mutable fault state or externally selectable calculation path.
template <class Function>
auto calculate_at(CalculationStage stage, std::optional<std::uint64_t> boundary,
                  Function&& function)
    -> std::expected<typename decltype(function())::value_type, CalculationError> {
#ifdef EPYCBENCH_RESULTS_CALCULATION_TESTING
    for (const auto& rule : calculation_test::rules)
        if (rule.enabled && rule.stage == stage && rule.historical == boundary.has_value() &&
            (!rule.boundary || rule.boundary == boundary))
            return std::unexpected{calculation_error(stage, rule.kind)};
#else
    (void)boundary;
#endif
    auto result = function();
    if (!result)
        return std::unexpected{classify(stage, result.error())};
    return std::move(*result);
}

// Validate the whole selected shape before any calculation/fault seam is reached.
inline auto valid_population(std::span<const runtime::PairwiseBlockAttemptEvidence> blocks,
                             std::optional<std::uint64_t> boundary = std::nullopt) noexcept
    -> bool {
    std::optional<std::size_t> family;
    bool found = false;
    for (const auto& block : blocks) {
        if (boundary && block.plan.block_id().value() > *boundary)
            break;
        if (block.completion != runtime::BlockAttemptCompletion::complete_valid)
            continue;
        found = true;
        if (block.observations.size() != 4)
            return false;
        std::array<unsigned, 2> roles{};
        for (const auto& factual : block.observations) {
            const auto index = factual.observation.index();
            if (family && *family != index)
                return false;
            family = index;
            const auto role =
                factual.slot.comparison_variant() == measurement::ComparisonVariantSlot::first ? 0U
                                                                                               : 1U;
            if (++roles[role] > 2)
                return false;
        }
        if (roles[0] != 2 || roles[1] != 2)
            return false;
    }
    return found;
}

template <class Observation, class Metric, class MakeMetric, class Representatives, class Effect>
auto rebuild_prefix(std::span<const runtime::PairwiseBlockAttemptEvidence> blocks,
                    std::uint64_t boundary, std::span<statistics::PairedLogEffect> output,
                    MakeMetric make_metric, Representatives representatives, Effect effect)
    -> std::expected<std::size_t, CalculationError> {
    using S = CalculationStage;
    if (!valid_population(blocks, boundary))
        return std::unexpected{
            calculation_error(S::primary_metric, CalculationFailureKind::structural)};
    std::size_t count{};
    for (const auto& block : blocks) {
        if (block.plan.block_id().value() > boundary)
            break;
        if (block.completion != runtime::BlockAttemptCompletion::complete_valid)
            continue;
        if (count == output.size())
            return std::unexpected{
                calculation_error(S::paired_effect, CalculationFailureKind::capacity)};
        std::array<std::optional<Metric>, 2> first{}, second{};
        std::size_t fi{}, si{};
        for (const auto& factual : block.observations) {
            const auto* observation = std::get_if<Observation>(&factual.observation);
            if (!observation)
                return std::unexpected{
                    calculation_error(S::primary_metric, CalculationFailureKind::structural)};
            auto metric = calculate_at(S::primary_metric, boundary,
                                       [&] { return make_metric(*observation); });
            if (!metric)
                return std::unexpected{metric.error()};
            if (factual.slot.comparison_variant() == measurement::ComparisonVariantSlot::first)
                first[fi++] = *metric;
            else
                second[si++] = *metric;
        }
        auto reps = calculate_at(S::block_representatives, boundary, [&] {
            return representatives(std::array<Metric, 2>{*first[0], *first[1]},
                                   std::array<Metric, 2>{*second[0], *second[1]});
        });
        if (!reps)
            return std::unexpected{reps.error()};
        auto paired = calculate_at(S::paired_effect, boundary, [&] { return effect(*reps); });
        if (!paired)
            return std::unexpected{paired.error()};
        output[count++] = *paired;
    }
    return count;
}
} // namespace epycbench::results::detail
