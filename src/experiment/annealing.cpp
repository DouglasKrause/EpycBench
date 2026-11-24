#include "experiment/annealing.hpp"

#include "epycbench/core/seed.hpp"
#include "epycbench/experiment/coordinator.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string>
#include <utility>

namespace epycbench::experiment {
namespace {

constexpr unsigned throughput_fraction_bits = 52U;
constexpr auto leading_significand_bit = std::uint64_t{1} << throughput_fraction_bits;
constexpr auto greatest_positive_finite = std::uint64_t{0x7fefffffffffffff};
constexpr auto certain_threshold = std::uint64_t{1} << 32U;

auto failure(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"experiment.annealing.invalid"},
        .message = "Annealing requires bounded cooling and eligible scores for the proposed move",
        .operation = "experiment.annealing",
        .os_error = {},
        .context = {{"classification", classification}},
    }};
}

auto temperature_at(AnnealingCooling cooling, std::uint64_t step) -> std::expected<std::uint32_t, core::Error> {
    if (cooling.minimum_temperature == 0U || cooling.minimum_temperature > cooling.initial_temperature ||
        cooling.initial_temperature > annealing_loss_scale || cooling.steps_per_halving == 0U ||
        cooling.steps_per_halving > maximum_attempts)
        return failure("cooling");
    const auto halvings = step / cooling.steps_per_halving;
    const auto decay = halvings >= std::numeric_limits<std::uint32_t>::digits
        ? 0U : cooling.initial_temperature >> halvings;
    return std::max(cooling.minimum_temperature, decay);
}

auto throughput_bits(double throughput) -> std::expected<std::uint64_t, core::Error> {
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);
    const auto bits = std::bit_cast<std::uint64_t>(throughput);
    if (bits == 0U || bits > greatest_positive_finite) return failure("throughput");
    return bits;
}

struct NormalizedThroughput {
    std::uint64_t significand;
    int exponent;
};

auto normalize(std::uint64_t bits) noexcept -> NormalizedThroughput {
    const auto exponent = static_cast<int>(bits >> throughput_fraction_bits);
    const auto fraction = bits & (leading_significand_bit - 1U);
    if (exponent != 0) return {fraction | leading_significand_bit, exponent};
    // Subnormals start with the minimum normal exponent and an explicit
    // significand. Normalize it so both values use the same exponent bias.
    const auto shift = std::countl_zero(fraction) - std::countl_zero(leading_significand_bit);
    return {fraction << shift, 1 - shift};
}

auto relative_loss(std::uint64_t incumbent, std::uint64_t candidate) noexcept -> std::uint32_t {
    const auto current = normalize(incumbent);
    const auto next = normalize(candidate);
    const auto exponent_gap = current.exponent - next.exponent;
    constexpr auto fractional_precision = std::countr_zero(annealing_loss_scale);
    if (exponent_gap > fractional_precision) return annealing_loss_scale;

    // Compute floor(2^20 * candidate / incumbent) by long division. Each
    // iteration doubles a remainder smaller than the 53-bit denominator.
    auto remaining = next.significand % current.significand;
    auto ratio = next.significand / current.significand;
    const auto places = fractional_precision - exponent_gap;
    for (int i{}; i < places; ++i) {
        ratio <<= 1U;
        remaining <<= 1U;
        if (remaining >= current.significand) {
            remaining -= current.significand;
            ++ratio;
        }
    }
    return annealing_loss_scale - static_cast<std::uint32_t>(ratio);
}

auto valid_lineage(const AnnealingStep& step) noexcept -> bool {
    if (!step.proposal || step.temperature == 0U || step.temperature > annealing_loss_scale ||
        step.proposal->ordinal == step.incumbent_ordinal)
        return false;
    const auto& proposal = *step.proposal;
    const bool initial = step.step == 0U && !step.incumbent_ordinal;
    switch (proposal.origin) {
    case HillClimbProposalOrigin::initial: return initial && !proposal.change;
    case HillClimbProposalOrigin::neighbor:
        return step.incumbent_ordinal && proposal.change && proposal.change->axis < maximum_genetic_axes &&
               proposal.change->before != proposal.change->after;
    case HillClimbProposalOrigin::restart: return !initial && !proposal.change;
    }
    return false;
}

} // namespace

auto propose_annealing_step(const GeneticSpace& space, std::uint64_t root_seed, std::uint64_t step,
                            AnnealingCooling cooling, std::optional<std::size_t> incumbent_ordinal,
                            std::span<const std::size_t> visited) -> std::expected<AnnealingStep, core::Error> {
    const auto temperature = temperature_at(cooling, step);
    if (!temperature) return std::unexpected{temperature.error()};
    const auto proposal_seed = core::derive_seed(root_seed, {core::SeedDomain::block_order,
        {{"annealing_profile", std::string{annealing_profile}}, {"phase", "proposal"}}});
    if (!proposal_seed) return std::unexpected{proposal_seed.error()};
    const auto incumbent = incumbent_ordinal ? std::optional{HillClimbIncumbent{*incumbent_ordinal}} : std::nullopt;
    auto batch = propose_hill_climb_batch(space, proposal_seed->value(), step, 1U, incumbent, visited);
    if (!batch) return std::unexpected{batch.error()};
    AnnealingStep result{step, *temperature, incumbent_ordinal, {}, batch->exhausted};
    if (!batch->proposals.empty()) result.proposal.emplace(std::move(batch->proposals.front()));
    return result;
}

auto decide_annealing_move(const AnnealingStep& step, std::uint64_t root_seed,
                           std::optional<AnnealingScore> incumbent, std::optional<AnnealingScore> candidate)
    -> std::expected<AnnealingDecision, core::Error> {
    const auto incumbent_ordinal = incumbent ? std::optional{incumbent->ordinal} : std::nullopt;
    if (!valid_lineage(step) || incumbent_ordinal != step.incumbent_ordinal ||
        (candidate && candidate->ordinal != step.proposal->ordinal))
        return failure("move_lineage");
    const auto current = incumbent ? throughput_bits(incumbent->throughput)
                                   : std::expected<std::uint64_t, core::Error>{0U};
    if (!current) return std::unexpected{current.error()};
    if (!candidate) return AnnealingDecision{};
    const auto next = throughput_bits(candidate->throughput);
    if (!next) return std::unexpected{next.error()};
    if (step.proposal->origin != HillClimbProposalOrigin::neighbor || *next >= *current)
        return AnnealingDecision{true, 0U, certain_threshold};

    const auto loss = relative_loss(*current, *next);
    const auto quotient = loss / step.temperature;
    const auto remainder = loss % step.temperature;
    const auto denominator = 2U * step.temperature;
    const auto threshold = quotient < 32U
        ? ((certain_threshold >> quotient) * (denominator - remainder)) / denominator : 0U;
    const auto random = core::derive_seed(root_seed, {core::SeedDomain::block_order,
        {{"annealing_profile", std::string{annealing_profile}}, {"phase", "acceptance"},
         {"step", std::to_string(step.step)}, {"incumbent", std::to_string(incumbent->ordinal)},
         {"candidate", std::to_string(candidate->ordinal)}}});
    if (!random) return std::unexpected{random.error()};
    const auto draw = static_cast<std::uint32_t>(random->value() >> 32U);
    return AnnealingDecision{draw < threshold, loss, threshold, draw};
}

} // namespace epycbench::experiment
