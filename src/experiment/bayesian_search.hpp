#pragma once

#include "epycbench/experiment/genetic.hpp"
#include "epycbench/experiment/local_search.hpp"

namespace epycbench::experiment {

inline constexpr std::string_view bayesian_proposal_profile = "epycbench.bayesian-categorical/v1";
inline constexpr std::string_view hybrid_proposal_profile = "epycbench.bayesian-local-hybrid/v1";
inline constexpr std::uint32_t bayesian_probability_scale = std::uint32_t{1} << 24U;
inline constexpr std::size_t bayesian_minimum_verified = 4U;

struct BayesianScore {
    std::size_t ordinal;
    double throughput;
};

enum class BayesianProposalPolicy { categorical, hybrid };
enum class BayesianProposalOrigin { exploration, surrogate, local };

struct BayesianProposal {
    std::size_t ordinal;
    BayesianProposalOrigin origin;
    std::optional<std::uint32_t> elite_probability_units;
    std::optional<std::size_t> parent{};
    std::optional<HillClimbAxisChange> change{};
    auto operator==(const BayesianProposal&) const -> bool = default;
};

struct BayesianStep {
    std::uint64_t step;
    std::size_t verified_count;
    std::size_t elite_count; // Zero unless the surrogate is fitted.
    std::optional<BayesianProposal> proposal;
    bool exhausted; // This step leaves no unseen ordinals to offer.
    auto operator==(const BayesianStep&) const -> bool = default;
};

// The caller supplies distinct verified positive throughput scores from one
// fixed problem, Build and topology cohort. Visited contains every earlier
// offered ordinal, including rejected or unscored cases. Each score must name
// a visited ordinal. GeneticSpace bounds the search to 4,096 cases and 32 axes.
//
// Explore before four verified scores and on zero-based steps 3, 7, 11, ... .
// Otherwise the elite quartile contains ceil(verified_count / 4) scores ordered
// by throughput descending, then by ordinal ascending. Each category has a
// Beta(1,1) prior on membership in that retained elite quartile. Quantize each
// posterior mean down to integer units of 2^-24 before averaging uniformly
// over the nontrivial axes and rounding the average down. The model estimates
// only marginal elite membership. The first candidate in the step's seeded
// permutation wins quantized ties and supplies the exploration choice.
[[nodiscard]] auto propose_bayesian_step(const GeneticSpace&, std::uint64_t root_seed,
    std::uint64_t step, std::span<const BayesianScore> verified = {},
    std::span<const std::size_t> visited = {}) -> std::expected<BayesianStep, core::Error>;

// On zero-based steps 1, 5, 9, ... the hybrid first tries an unseen neighbor
// of the highest-throughput verified ordinal, using the same ordinal tie rule.
// Its permutation uses the hybrid profile, root seed, step and parent ordinal.
// If no such neighbor exists, fall back to the original categorical policy.
[[nodiscard]] auto propose_bayesian_step(const GeneticSpace&, std::uint64_t root_seed,
    std::uint64_t step, std::span<const BayesianScore> verified,
    std::span<const std::size_t> visited, BayesianProposalPolicy)
    -> std::expected<BayesianStep, core::Error>;

} // namespace epycbench::experiment
