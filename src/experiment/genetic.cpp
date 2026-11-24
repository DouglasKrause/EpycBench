#include "epycbench/experiment/genetic.hpp"

#include "epycbench/experiment/coordinator.hpp"

#include <algorithm>
#include <random>
#include <utility>

namespace epycbench::experiment {
namespace {

auto failure(std::string_view classification) -> std::unexpected<core::Error> {
    return std::unexpected{core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"experiment.genetic.invalid"},
        .message = "Genetic search could not propose a bounded generation",
        .operation = "experiment.genetic",
        .os_error = {},
        .context = {{"classification", classification}},
    }};
}

void decode_coordinates(std::size_t ordinal, std::span<const std::size_t> cardinalities,
                        std::span<std::size_t> coordinates) noexcept {
    for (auto remaining = cardinalities.size(); remaining != 0; --remaining) {
        const auto axis = remaining - 1;
        coordinates[axis] = ordinal % cardinalities[axis];
        ordinal /= cardinalities[axis];
    }
}

auto encode_coordinates(std::span<const std::size_t> cardinalities,
                        std::span<const std::size_t> coordinates) noexcept -> std::size_t {
    std::size_t ordinal{};
    for (std::size_t axis{}; axis < cardinalities.size(); ++axis)
        ordinal = ordinal * cardinalities[axis] + coordinates[axis];
    return ordinal;
}

struct RejectionLimit {};

class GenerationSampler {
  public:
    GenerationSampler(std::uint64_t root_seed, std::uint64_t generation)
        : random_{seeded_engine(root_seed, generation)} {}

    [[nodiscard]] auto uniform(std::size_t size) -> std::size_t {
        const auto bound = static_cast<std::uint64_t>(size);
        const auto cutoff = (std::uint64_t{0} - bound) % bound;
        for (unsigned draw{}; draw < 32U; ++draw) {
            const auto word = random_();
            if (word >= cutoff) return static_cast<std::size_t>(word % bound);
        }
        throw RejectionLimit{};
    }

  private:
    static auto seeded_engine(std::uint64_t root_seed, std::uint64_t generation) -> std::mt19937_64 {
        constexpr std::uint32_t profile_tag = 0x47454e31U; // GEN1
        const std::array words{profile_tag, static_cast<std::uint32_t>(root_seed),
            static_cast<std::uint32_t>(root_seed >> 32U), static_cast<std::uint32_t>(generation),
            static_cast<std::uint32_t>(generation >> 32U)};
        std::seed_seq seed{words.begin(), words.end()};
        return std::mt19937_64{seed};
    }

    std::mt19937_64 random_;
};

auto tournament(GenerationSampler& sampler, std::size_t parent_count,
                std::optional<std::size_t> excluded_parent = {}) -> std::size_t {
    const auto draw = [&] {
        const auto index = sampler.uniform(parent_count - excluded_parent.has_value());
        return excluded_parent && index >= *excluded_parent ? index + 1U : index;
    };
    const auto first = draw();
    const auto second = draw();
    return std::min(first, second); // Parents are already sorted by rank and ordinal.
}

auto offspring(std::span<const std::size_t> cardinalities, std::span<const std::size_t> mutable_axes,
               GenerationSampler& sampler, std::size_t first_parent, std::size_t second_parent,
               std::uint16_t mutation_per_mille) -> GeneticProposal {
    std::array<std::size_t, maximum_genetic_axes> digits{}, second{};
    decode_coordinates(first_parent, cardinalities, digits);
    decode_coordinates(second_parent, cardinalities, second);
    std::uint32_t second_parent_axes{};
    for (std::size_t axis{}; axis < cardinalities.size(); ++axis) {
        if (sampler.uniform(2) == 0) continue;
        digits[axis] = second[axis];
        second_parent_axes |= std::uint32_t{1} << axis;
    }
    const auto pre_mutation = encode_coordinates(cardinalities, digits);
    auto selected = pre_mutation;
    std::optional<GeneticMutation> mutation;
    if (!mutable_axes.empty() && mutation_per_mille != 0 && sampler.uniform(1000) < mutation_per_mille) {
        const auto axis = mutable_axes[sampler.uniform(mutable_axes.size())];
        const auto before = digits[axis];
        auto after = sampler.uniform(cardinalities[axis] - 1U);
        if (after >= before) ++after;
        digits[axis] = after;
        selected = encode_coordinates(cardinalities, digits);
        mutation.emplace(axis, before, after);
    }
    return {selected, GeneticProposalOrigin::offspring,
            GeneticCrossover{first_parent, second_parent, second_parent_axes, pre_mutation}, mutation};
}

enum class OrdinalState : std::uint8_t { unseen, visited, parent };

} // namespace

auto GeneticSpace::create(std::span<const std::size_t> cardinalities)
    -> std::expected<GeneticSpace, core::Error> {
    if (cardinalities.size() > maximum_genetic_axes) return failure("axis_limit");
    std::size_t size{1};
    for (const auto cardinality : cardinalities) {
        if (cardinality == 0 || cardinality > maximum_attempts / size)
            return failure("space_size");
        size *= cardinality;
    }
    std::array<std::size_t, maximum_genetic_axes> stock{};
    std::ranges::copy(cardinalities, stock.begin());
    return GeneticSpace{stock, cardinalities.size(), size};
}

auto GeneticSpace::coordinates(std::size_t ordinal) const
    -> std::expected<std::vector<std::size_t>, core::Error> {
    if (ordinal >= size_) return failure("ordinal");
    std::vector<std::size_t> result(axes_);
    decode_coordinates(ordinal, cardinalities(), result);
    return result;
}

auto GeneticSpace::ordinal(std::span<const std::size_t> coordinates) const
    -> std::expected<std::size_t, core::Error> {
    if (coordinates.size() != axes_) return failure("coordinate_count");
    for (std::size_t axis{}; axis < axes_; ++axis)
        if (coordinates[axis] >= cardinalities_[axis]) return failure("coordinate");
    return encode_coordinates(cardinalities(), coordinates);
}

auto propose_genetic_generation(const GeneticSpace& space, std::uint64_t root_seed,
                                std::uint64_t generation, GeneticProposalOptions options,
                                std::span<const RankedGeneticParent> parents,
                                std::span<const std::size_t> visited)
    -> std::expected<GeneticGeneration, core::Error> {
    if (options.population == 0 || options.population > maximum_attempts ||
        options.mutation_per_mille > 1000 || parents.size() > space.size() ||
        visited.size() > space.size() || (generation == 0 && !parents.empty()))
        return failure("generation_options");
    try {
        std::vector<OrdinalState> state(space.size(), OrdinalState::unseen);
        for (const auto ordinal : visited) {
            if (ordinal >= state.size()) return failure("visited_ordinal");
            state[ordinal] = OrdinalState::visited;
        }
        for (const auto& parent : parents) {
            if (parent.ordinal >= state.size() || state[parent.ordinal] == OrdinalState::parent)
                return failure("parent_ordinal");
            state[parent.ordinal] = OrdinalState::parent;
        }

        const auto available = static_cast<std::size_t>(std::ranges::count(state, OrdinalState::unseen));
        const auto target = std::min(available, options.population);
        GeneticGeneration result{.generation = generation, .proposals = {}, .exhausted = target == available};
        if (target == 0) return result;
        result.proposals.reserve(target);
        GenerationSampler sampler{root_seed, generation};
        const auto cardinalities = space.cardinalities();
        if (!parents.empty()) {
            std::vector<RankedGeneticParent> ranked(parents.begin(), parents.end());
            std::ranges::sort(ranked, {}, [](const auto& parent) {
                return std::pair{parent.rank, parent.ordinal};
            });
            std::array<std::size_t, maximum_genetic_axes> mutable_axes{};
            std::size_t mutable_count{};
            for (std::size_t axis{}; axis < cardinalities.size(); ++axis)
                if (cardinalities[axis] > 1) mutable_axes[mutable_count++] = axis;

            constexpr std::size_t draws_per_slot = 8;
            const auto maximum_draws = target * draws_per_slot;
            while (result.offspring_draws < maximum_draws && result.proposals.size() < target) {
                const auto first = tournament(sampler, ranked.size());
                const auto second = ranked.size() == 1 ? first : tournament(sampler, ranked.size(), first);
                auto proposal = offspring(cardinalities, std::span{mutable_axes}.first(mutable_count),
                    sampler, ranked[first].ordinal, ranked[second].ordinal, options.mutation_per_mille);
                ++result.offspring_draws;
                if (state[proposal.ordinal] != OrdinalState::unseen) {
                    ++result.duplicate_offspring;
                    continue;
                }
                state[proposal.ordinal] = OrdinalState::visited;
                result.proposals.push_back(std::move(proposal));
            }
        }

        if (result.proposals.size() < target) {
            std::vector<std::size_t> remaining;
            remaining.reserve(available - result.proposals.size());
            for (std::size_t ordinal{}; ordinal < state.size(); ++ordinal)
                if (state[ordinal] == OrdinalState::unseen) remaining.push_back(ordinal);
            const auto origin = generation == 0 ? GeneticProposalOrigin::initial : GeneticProposalOrigin::exploration;
            const auto slots = target - result.proposals.size();
            for (std::size_t index{}; index < slots; ++index) {
                const auto selected = index + sampler.uniform(remaining.size() - index);
                std::swap(remaining[index], remaining[selected]);
                result.proposals.push_back({remaining[index], origin, {}, {}});
            }
        }
        return result;
    } catch (const RejectionLimit&) {
        return failure("sampling_limit");
    }
}

} // namespace epycbench::experiment
