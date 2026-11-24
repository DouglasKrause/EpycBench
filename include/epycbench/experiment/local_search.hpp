#pragma once

#include "epycbench/experiment/genetic.hpp"

namespace epycbench::experiment {

inline constexpr std::string_view hill_climb_proposal_profile = "epycbench.hill-climb-proposals/v1";

struct HillClimbIncumbent {
    std::size_t ordinal;
    auto operator==(const HillClimbIncumbent&) const -> bool = default;
};

enum class HillClimbProposalOrigin { initial, neighbor, restart };

struct HillClimbAxisChange {
    std::size_t axis;
    std::size_t before;
    std::size_t after;
    auto operator==(const HillClimbAxisChange&) const -> bool = default;
};

struct HillClimbProposal {
    std::size_t ordinal;
    HillClimbProposalOrigin origin;
    std::optional<HillClimbAxisChange> change;
    auto operator==(const HillClimbProposal&) const -> bool = default;
};

struct HillClimbBatch {
    std::uint64_t step;
    std::optional<HillClimbIncumbent> incumbent;
    std::size_t unseen_neighbors;
    std::vector<HillClimbProposal> proposals;
    bool exhausted; // Every remaining ordinal is in this batch.
    auto operator==(const HillClimbBatch&) const -> bool = default;
};

// The caller supplies a distinct set of previously proposed ordinals, including
// unresolved or rejected proposals. An incumbent must be in that set and backed
// by a verified Result from the same comparison cohort. The caller owns scoring
// and accepts a neighbor move only when the comparable objective improves.
//
// A neighbor changes one categorical axis to any different value. If unseen
// neighbors exist, this batch contains only neighbors. Otherwise it samples
// the remaining unseen space for a restart. The caller may establish a fresh
// incumbent from a verified restart. With no incumbent, global sampling gives
// initial proposals at step zero and restart proposals later.
// Every selection is made without replacement using the root seed and step.
[[nodiscard]] auto propose_hill_climb_batch(const GeneticSpace&, std::uint64_t root_seed,
    std::uint64_t step, std::size_t maximum_proposals, std::optional<HillClimbIncumbent> incumbent = {},
    std::span<const std::size_t> visited = {}) -> std::expected<HillClimbBatch, core::Error>;

} // namespace epycbench::experiment
