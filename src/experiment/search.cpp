#include "epycbench/experiment/search.hpp"

#include "epycbench/experiment/coordinator.hpp"

#include <numeric>
#include <random>
#include <utility>

namespace epycbench::experiment {

auto strategy_token(SearchStrategy strategy) noexcept -> std::string_view {
    switch (strategy) {
    case SearchStrategy::enumeration: return "enumeration";
    case SearchStrategy::seeded_random: return "seeded-random";
    }
    return {};
}

auto candidate_order(SearchStrategy strategy, std::size_t candidate_count, std::uint64_t seed)
    -> std::expected<std::vector<std::size_t>, core::Error> {
    if (candidate_count == 0U || candidate_count > maximum_attempts || strategy_token(strategy).empty()) {
        return std::unexpected{core::Error{
            .category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"experiment.search.invalid_space"},
            .message = "A search requires a recognized strategy and a bounded nonempty space",
            .operation = "experiment.search", .os_error = {}, .context = {}}};
    }
    std::vector<std::size_t> order(candidate_count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (strategy == SearchStrategy::seeded_random) {
        std::mt19937_64 random{seed};
        // Use an explicit sampler so a replay does not depend on the standard
        // library implementation of uniform_int_distribution or shuffle.
        for (std::size_t remaining = order.size(); remaining > 1U; --remaining) {
            const auto bound = static_cast<std::uint64_t>(remaining);
            const auto cutoff = (std::uint64_t{0} - bound) % bound;
            auto draw = random();
            while (draw < cutoff) draw = random();
            std::swap(order[remaining - 1U], order[static_cast<std::size_t>(draw % bound)]);
        }
    }
    return order;
}

} // namespace epycbench::experiment
