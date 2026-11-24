#include "experiment/bayesian_search.hpp"

#include "epycbench/core/seed.hpp"
#include "epycbench/experiment/search.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string>

namespace epycbench::experiment {
namespace {

auto failure(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"experiment.bayesian.invalid"},
        .message = "Bayesian search requires bounded visited ordinals and verified throughput scores",
        .operation = "experiment.bayesian",
        .os_error = {},
        .context = {{"classification", classification}},
    }};
}

enum class OrdinalState : std::uint8_t { unseen, visited, scored };

struct RankedScore {
    std::size_t ordinal;
    std::uint64_t throughput_bits;
};

auto ranks_before(const RankedScore& first, const RankedScore& second) noexcept -> bool {
    if (first.throughput_bits != second.throughput_bits)
        return first.throughput_bits > second.throughput_bits;
    return first.ordinal < second.ordinal;
}

struct Category {
    std::uint64_t observed{};
    std::uint64_t elite{};
    std::uint32_t probability_units{};
};

template <class UseCategory>
void visit_categories(std::size_t ordinal, std::span<const std::size_t> cardinalities,
                      std::span<const std::size_t> offsets, UseCategory use) {
    for (auto remaining = cardinalities.size(); remaining != 0U; --remaining) {
        const auto axis = remaining - 1U;
        const auto radix = cardinalities[axis];
        if (radix == 1U) continue;
        use(offsets[axis] + ordinal % radix);
        ordinal /= radix;
    }
}

auto select_surrogate(std::span<const std::size_t> cardinalities, std::size_t elite_count,
                      std::span<const RankedScore> ranked, std::span<const OrdinalState> states,
                      std::span<const std::size_t> order) -> std::optional<BayesianProposal> {
    std::array<std::size_t, maximum_genetic_axes> offsets{};
    std::size_t category_count{}, nontrivial_axes{};
    for (std::size_t axis{}; axis < cardinalities.size(); ++axis) {
        if (cardinalities[axis] == 1U) continue;
        offsets[axis] = category_count;
        category_count += cardinalities[axis];
        ++nontrivial_axes;
    }
    // The product of the nontrivial radices bounds their sum. A fitted step
    // has at least four distinct observed cases, so it has a nontrivial axis.
    std::vector<Category> categories(category_count);
    for (std::size_t rank{}; rank < ranked.size(); ++rank) {
        visit_categories(ranked[rank].ordinal, cardinalities, offsets, [&](std::size_t index) {
            ++categories[index].observed;
            categories[index].elite += rank < elite_count;
        });
    }
    for (auto& category : categories) {
        category.probability_units = static_cast<std::uint32_t>(
            bayesian_probability_scale * (category.elite + 1U) / (category.observed + 2U));
    }

    std::optional<BayesianProposal> best;
    for (const auto ordinal : order) {
        if (states[ordinal] != OrdinalState::unseen) continue;
        std::uint64_t sum{};
        visit_categories(ordinal, cardinalities, offsets, [&](std::size_t index) {
            sum += categories[index].probability_units;
        });
        const auto units = static_cast<std::uint32_t>(sum / nontrivial_axes);
        if (!best || units > *best->elite_probability_units)
            best = BayesianProposal{ordinal, BayesianProposalOrigin::surrogate, units};
    }
    return best;
}

auto proposal_order(std::size_t count, std::uint64_t root_seed, std::uint64_t step,
                    std::optional<std::size_t> parent = {})
    -> std::expected<std::vector<std::size_t>, core::Error> {
    const auto profile = parent ? hybrid_proposal_profile : bayesian_proposal_profile;
    core::SeedDerivationKey key{core::SeedDomain::block_order,
        {{"proposer", std::string{profile}}, {"step", std::to_string(step)}}};
    if (parent) key.components.push_back({"parent_ordinal", std::to_string(*parent)});
    const auto seed = core::derive_seed(root_seed, key);
    if (!seed) return std::unexpected{seed.error()};
    return candidate_order(SearchStrategy::seeded_random, count, seed->value());
}

auto one_axis_change(std::size_t parent, std::size_t candidate,
                     std::span<const std::size_t> cardinalities) noexcept
    -> std::optional<HillClimbAxisChange> {
    std::optional<HillClimbAxisChange> change;
    for (auto remaining = cardinalities.size(); remaining != 0U; --remaining) {
        const auto axis = remaining - 1U;
        const auto radix = cardinalities[axis];
        if (radix == 1U) continue;
        const auto before = parent % radix;
        const auto after = candidate % radix;
        if (before != after) {
            if (change) return std::nullopt;
            change = HillClimbAxisChange{axis, before, after};
        }
        parent /= radix;
        candidate /= radix;
    }
    return change;
}

} // namespace

auto propose_bayesian_step(const GeneticSpace& space, std::uint64_t root_seed,
                           std::uint64_t step, std::span<const BayesianScore> verified,
                           std::span<const std::size_t> visited) -> std::expected<BayesianStep, core::Error> {
    return propose_bayesian_step(space, root_seed, step, verified, visited,
                                 BayesianProposalPolicy::categorical);
}

auto propose_bayesian_step(const GeneticSpace& space, std::uint64_t root_seed,
                           std::uint64_t step, std::span<const BayesianScore> verified,
                           std::span<const std::size_t> visited, BayesianProposalPolicy policy)
    -> std::expected<BayesianStep, core::Error> {
    if (visited.size() > space.size() || verified.size() > visited.size())
        return failure("count");
    std::vector<OrdinalState> states(space.size(), OrdinalState::unseen);
    for (const auto ordinal : visited) {
        if (ordinal >= space.size() || states[ordinal] != OrdinalState::unseen)
            return failure("visited_ordinal");
        states[ordinal] = OrdinalState::visited;
    }

    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);
    constexpr auto greatest_positive_finite = std::uint64_t{0x7fefffffffffffff};
    std::vector<RankedScore> ranked;
    ranked.reserve(verified.size());
    for (const auto& score : verified) {
        if (score.ordinal >= space.size() || states[score.ordinal] != OrdinalState::visited)
            return failure("score_ordinal");
        const auto bits = std::bit_cast<std::uint64_t>(score.throughput);
        if (bits == 0U || bits > greatest_positive_finite) return failure("throughput");
        states[score.ordinal] = OrdinalState::scored;
        ranked.push_back({score.ordinal, bits});
    }

    const auto available = space.size() - visited.size();
    BayesianStep result{step, verified.size(), 0U, {}, available <= 1U};
    if (available == 0U) return result;

    if (policy == BayesianProposalPolicy::hybrid && step % 4U == 1U && !ranked.empty()) {
        const auto parent = std::ranges::min_element(ranked, ranks_before)->ordinal;
        const auto order = proposal_order(space.size(), root_seed, step, parent);
        if (!order) return std::unexpected{order.error()};
        for (const auto ordinal : *order) {
            if (states[ordinal] != OrdinalState::unseen) continue;
            if (const auto change = one_axis_change(parent, ordinal, space.cardinalities())) {
                result.proposal = BayesianProposal{ordinal, BayesianProposalOrigin::local, {}, parent, change};
                return result;
            }
        }
    }

    const auto order = proposal_order(space.size(), root_seed, step);
    if (!order) return std::unexpected{order.error()};
    if (verified.size() < bayesian_minimum_verified || step % 4U == 3U) {
        const auto first = std::ranges::find_if(*order, [&](std::size_t ordinal) {
            return states[ordinal] == OrdinalState::unseen;
        });
        result.proposal = BayesianProposal{*first, BayesianProposalOrigin::exploration, {}};
        return result;
    }

    std::ranges::sort(ranked, ranks_before);
    result.elite_count = (verified.size() + 3U) / 4U;
    result.proposal = select_surrogate(space.cardinalities(), result.elite_count, ranked, states, *order);
    return result;
}

} // namespace epycbench::experiment
