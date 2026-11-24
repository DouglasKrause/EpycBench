#pragma once

#include "epycbench/experiment/local_search.hpp"

namespace epycbench::experiment {

inline constexpr std::string_view annealing_profile = "epycbench.simulated-annealing/v1";
inline constexpr std::uint32_t annealing_loss_scale = std::uint32_t{1} << 20U;

struct AnnealingCooling {
    // Temperatures are positive units of relative throughput loss scaled by
    // annealing_loss_scale. Cap the initial temperature at that scale.
    // T(step) = max(minimum_temperature, initial_temperature >> (step / steps_per_halving)).
    // The number of steps per halving is between one and the attempt limit.
    std::uint32_t initial_temperature;
    std::uint32_t minimum_temperature;
    std::uint32_t steps_per_halving;
    auto operator==(const AnnealingCooling&) const -> bool = default;
};

struct AnnealingStep {
    std::uint64_t step;
    std::uint32_t temperature;
    std::optional<std::size_t> incumbent_ordinal;
    // Annealing and hill climbing share the same single-axis move lineage.
    std::optional<HillClimbProposal> proposal;
    bool exhausted; // This step leaves no unseen cases to propose.
    auto operator==(const AnnealingStep&) const -> bool = default;
};

// Propose one unseen neighbor. With no unseen neighbor, propose a single
// seeded initial or restart case. Every prior proposal belongs in visited
// regardless of its eligibility or acceptance.
[[nodiscard]] auto propose_annealing_step(const GeneticSpace&, std::uint64_t root_seed,
    std::uint64_t step, AnnealingCooling, std::optional<std::size_t> incumbent_ordinal = {},
    std::span<const std::size_t> visited = {}) -> std::expected<AnnealingStep, core::Error>;

struct AnnealingScore {
    std::size_t ordinal;
    double throughput;
};

struct AnnealingDecision {
    bool accepted{};
    std::uint32_t loss_units{};
    // A 32-bit uniform draw is accepted when it is strictly less than this
    // threshold. The value 2^32 represents certainty.
    std::uint64_t threshold{};
    std::optional<std::uint32_t> draw{};
    auto operator==(const AnnealingDecision&) const -> bool = default;
};

// The caller provides only eligible verified throughput for the same fixed
// problem, Build and topology cohort. Maximizing positive throughput also
// minimizes seconds per task. A missing score leaves the movable incumbent
// unchanged. Track the best verified score separately from that incumbent.
//
// Downhill loss is L = ceil(2^20 * (1 - candidate / incumbent)). For
// q = L / temperature and r = L % temperature use
// floor(2^(32 - q) * (2 * temperature - r) / (2 * temperature)) when q < 32,
// and zero afterward. This interpolates between consecutive powers of two
// with integer arithmetic. Initial, restart and non-worse moves with a
// verified score are accepted with certainty.
[[nodiscard]] auto decide_annealing_move(const AnnealingStep&, std::uint64_t root_seed,
    std::optional<AnnealingScore> incumbent, std::optional<AnnealingScore> candidate)
    -> std::expected<AnnealingDecision, core::Error>;

} // namespace epycbench::experiment
