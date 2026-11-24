#include "epycbench/kernel/reduction_tree.hpp"

#include <algorithm>
#include <map>
#include <new>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace epycbench::kernel {
namespace {

template<class LocalityKey>
auto aggregate(ReductionLevelKind kind, std::span<const ReductionRankLocality> ranks,
               std::vector<std::size_t>& active, LocalityKey key_for) -> ReductionLevel {
    ReductionLevel level{kind, {}};
    level.groups.reserve(active.size());
    std::map<typename decltype(key_for(ranks.front()))::value_type, std::size_t> by_locality;
    for (const auto rank : active) {
        if (const auto key = key_for(ranks[rank])) {
            const auto [entry, inserted] = by_locality.try_emplace(*key, level.groups.size());
            if (!inserted) {
                level.groups[entry->second].children.push_back(rank);
                continue;
            }
        }
        level.groups.push_back({{rank}});
    }

    // Iterating the incoming leaders in order also fixes the group order.
    active.clear();
    for (const auto& group : level.groups) active.push_back(group.leader());
    return level;
}

} // namespace

auto ReductionTreePlan::create(std::span<const ReductionRankLocality> ranks,
                               std::size_t maximum_ranks) noexcept
    -> std::expected<ReductionTreePlan, ReductionTreeError> {
    if (ranks.empty()) return std::unexpected{ReductionTreeError::empty_cohort};
    if (ranks.size() > maximum_ranks) return std::unexpected{ReductionTreeError::rank_limit_exceeded};

    try {
        std::vector<std::size_t> active(ranks.size());
        std::iota(active.begin(), active.end(), std::size_t{0});

        auto llc = aggregate(ReductionLevelKind::llc, ranks, active, [](const auto& locality) {
            return locality.llc.transform([&](auto cache) {
                return std::tuple{locality.package, locality.numa, cache};
            });
        });
        auto numa = aggregate(ReductionLevelKind::numa, ranks, active, [](const auto& locality) {
            return locality.numa.transform([&](auto node) {
                return std::pair{locality.package, node};
            });
        });
        auto package = aggregate(ReductionLevelKind::package, ranks, active, [](const auto& locality) {
            return std::optional{locality.package};
        });
        ReductionLevel cohort{ReductionLevelKind::cohort, {}};
        cohort.groups.push_back({std::move(active)});
        return ReductionTreePlan{ranks.size(),
            {std::move(llc), std::move(numa), std::move(package), std::move(cohort)}};
    } catch (const std::length_error&) {
        return std::unexpected{ReductionTreeError::rank_limit_exceeded};
    } catch (const std::bad_alloc&) {
        return std::unexpected{ReductionTreeError::allocation_failure};
    }
}

auto ReductionTreePlan::from_levels(std::size_t workers, std::array<ReductionLevel, 4> levels,
                                    std::size_t maximum_ranks) noexcept
    -> std::expected<ReductionTreePlan, ReductionTreeError> {
    using Error = ReductionTreeError;
    if (!workers) return std::unexpected{Error::empty_cohort};
    if (workers > maximum_ranks) return std::unexpected{Error::rank_limit_exceeded};
    try {
        std::vector<std::size_t> active(workers);
        std::iota(active.begin(), active.end(), std::size_t{0});
        std::vector<std::size_t> children;
        children.reserve(workers);
        constexpr std::array kinds{ReductionLevelKind::llc, ReductionLevelKind::numa,
                                    ReductionLevelKind::package, ReductionLevelKind::cohort};
        for (std::size_t index{}; index < kinds.size(); ++index) {
            const auto& level = levels[index];
            if (level.kind != kinds[index] || level.groups.empty() || level.groups.size() > active.size())
                return std::unexpected{Error::invalid_levels};
            children.clear();
            std::optional<std::size_t> previous_leader;
            for (const auto& group : level.groups) {
                if (group.children.empty() || group.children.size() > active.size() - children.size() ||
                    (previous_leader && group.leader() <= *previous_leader) ||
                    !std::ranges::is_sorted(group.children))
                    return std::unexpected{Error::invalid_levels};
                previous_leader = group.leader();
                children.insert(children.end(), group.children.begin(), group.children.end());
            }
            std::ranges::sort(children);
            if (children != active) return std::unexpected{Error::invalid_levels};
            active.clear();
            for (const auto& group : level.groups) active.push_back(group.leader());
        }
        if (active.size() != 1U) return std::unexpected{Error::invalid_levels};
        return ReductionTreePlan{workers, std::move(levels)};
    } catch (const std::length_error&) {
        return std::unexpected{Error::rank_limit_exceeded};
    } catch (const std::bad_alloc&) {
        return std::unexpected{Error::allocation_failure};
    }
}

} // namespace epycbench::kernel
