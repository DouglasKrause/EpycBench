#pragma once

#include "epycbench/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace epycbench::experiment {

inline constexpr std::string_view genetic_proposal_profile = "epycbench.genetic-proposals/v1";
inline constexpr std::size_t maximum_genetic_axes = 32;

// Cardinalities are in ConfigurationSpace axis order. The last axis changes
// fastest, and a space with no axes contains just ordinal zero.
class GeneticSpace {
  public:
    [[nodiscard]] static auto create(std::span<const std::size_t> cardinalities)
        -> std::expected<GeneticSpace, core::Error>;
    [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] auto cardinalities() const noexcept -> std::span<const std::size_t> {
        return std::span{cardinalities_}.first(axes_);
    }
    [[nodiscard]] auto coordinates(std::size_t ordinal) const
        -> std::expected<std::vector<std::size_t>, core::Error>;
    [[nodiscard]] auto ordinal(std::span<const std::size_t> coordinates) const
        -> std::expected<std::size_t, core::Error>;

  private:
    GeneticSpace(std::array<std::size_t, maximum_genetic_axes> cardinalities,
                 std::size_t axes, std::size_t size) noexcept
        : cardinalities_{cardinalities}, axes_{axes}, size_{size} {}

    std::array<std::size_t, maximum_genetic_axes> cardinalities_;
    std::size_t axes_;
    std::size_t size_;
};

struct RankedGeneticParent {
    std::size_t ordinal;
    std::size_t rank;
};

struct GeneticProposalOptions {
    std::size_t population{};
    std::uint16_t mutation_per_mille{250};
};

enum class GeneticProposalOrigin { initial, offspring, exploration };

struct GeneticCrossover {
    std::size_t first_parent;
    std::size_t second_parent;
    std::uint32_t second_parent_axes;
    std::size_t ordinal; // Before mutation.
    auto operator==(const GeneticCrossover&) const -> bool = default;
};

struct GeneticMutation {
    std::size_t axis;
    std::size_t before;
    std::size_t after;
    auto operator==(const GeneticMutation&) const -> bool = default;
};

struct GeneticProposal {
    std::size_t ordinal;
    GeneticProposalOrigin origin;
    std::optional<GeneticCrossover> crossover;
    std::optional<GeneticMutation> mutation;
    auto operator==(const GeneticProposal&) const -> bool = default;
};

struct GeneticGeneration {
    std::uint64_t generation;
    std::vector<GeneticProposal> proposals;
    std::size_t offspring_draws{};
    std::size_t duplicate_offspring{};
    bool exhausted{};
    auto operator==(const GeneticGeneration&) const -> bool = default;
};

// Generation zero takes no parents. Later generations use two-way tournaments
// (lower rank first, ordinal breaks ties), uniform crossover, and optional
// single-gene mutation. At most eight offspring draws per requested slot are
// tried. Remaining slots are sampled from unseen ordinals without replacement.
// Every supplied parent and visited ordinal is excluded from new proposals.
// The caller admits comparable scored parents and resolves each new proposal.
// Record rejected proposals as visited without charging a measurement attempt.
[[nodiscard]] auto propose_genetic_generation(const GeneticSpace&, std::uint64_t root_seed,
    std::uint64_t generation, GeneticProposalOptions, std::span<const RankedGeneticParent> parents = {},
    std::span<const std::size_t> visited = {}) -> std::expected<GeneticGeneration, core::Error>;

} // namespace epycbench::experiment
