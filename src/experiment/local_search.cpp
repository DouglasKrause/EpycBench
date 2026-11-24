#include "epycbench/experiment/local_search.hpp"

#include "epycbench/core/seed.hpp"
#include "epycbench/experiment/coordinator.hpp"
#include "epycbench/experiment/search.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace epycbench::experiment {
namespace {

auto failure(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"experiment.hill_climb.invalid"},
        .message = "Hill climbing requires a bounded proposal count and valid visited ordinals",
        .operation = "experiment.hill_climb",
        .os_error = {},
        .context = {{"classification", classification}},
    }};
}

auto neighbor_changes(const GeneticSpace& space, std::size_t incumbent)
    -> std::expected<std::vector<std::optional<HillClimbAxisChange>>, core::Error> {
    auto coordinates = space.coordinates(incumbent);
    if (!coordinates) return std::unexpected{coordinates.error()};
    std::vector<std::optional<HillClimbAxisChange>> changes(space.size());
    const auto cardinalities = space.cardinalities();
    for (std::size_t axis{}; axis < cardinalities.size(); ++axis) {
        auto& choice = (*coordinates)[axis];
        const auto before = choice;
        for (std::size_t after{}; after < cardinalities[axis]; ++after) {
            if (after == before) continue;
            choice = after;
            const auto ordinal = space.ordinal(*coordinates);
            if (!ordinal) return std::unexpected{ordinal.error()};
            changes[*ordinal] = HillClimbAxisChange{axis, before, after};
        }
        choice = before;
    }
    return changes;
}

} // namespace

auto propose_hill_climb_batch(const GeneticSpace& space, std::uint64_t root_seed, std::uint64_t step,
                              std::size_t maximum_proposals, std::optional<HillClimbIncumbent> incumbent,
                              std::span<const std::size_t> visited) -> std::expected<HillClimbBatch, core::Error> {
    if (maximum_proposals == 0 || maximum_proposals > maximum_attempts || visited.size() > space.size())
        return failure("batch_limit");
    std::vector<bool> seen(space.size());
    for (const auto ordinal : visited) {
        if (ordinal >= space.size() || seen[ordinal]) return failure("visited_ordinal");
        seen[ordinal] = true;
    }
    if (incumbent && (incumbent->ordinal >= space.size() || !seen[incumbent->ordinal]))
        return failure("incumbent_ordinal");

    const auto available = space.size() - visited.size();
    HillClimbBatch batch{step, incumbent, 0U, {}, available == 0U};
    if (batch.exhausted) return batch;
    const auto target = std::min(available, maximum_proposals);
    batch.proposals.reserve(target);

    const auto seed = core::derive_seed(root_seed, {core::SeedDomain::block_order,
        {{"proposer", std::string{hill_climb_proposal_profile}}, {"step", std::to_string(step)}}});
    if (!seed) return std::unexpected{seed.error()};
    auto order = candidate_order(SearchStrategy::seeded_random, space.size(), seed->value());
    if (!order) return std::unexpected{std::move(order.error())};
    if (incumbent) {
        auto changes = neighbor_changes(space, incumbent->ordinal);
        if (!changes) return std::unexpected{std::move(changes.error())};
        for (const auto ordinal : *order) {
            const auto& change = (*changes)[ordinal];
            if (seen[ordinal] || !change) continue;
            ++batch.unseen_neighbors;
            if (batch.proposals.size() < target)
                batch.proposals.push_back({ordinal, HillClimbProposalOrigin::neighbor, change});
        }
    }
    if (batch.unseen_neighbors == 0U) {
        const auto origin = step == 0U && !incumbent ? HillClimbProposalOrigin::initial
                                                    : HillClimbProposalOrigin::restart;
        for (const auto ordinal : *order) {
            if (seen[ordinal]) continue;
            batch.proposals.push_back({ordinal, origin, {}});
            if (batch.proposals.size() == target) break;
        }
    }
    batch.exhausted = batch.proposals.size() == available;
    return batch;
}

} // namespace epycbench::experiment
