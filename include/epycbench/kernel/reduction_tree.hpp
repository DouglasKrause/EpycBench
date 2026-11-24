#pragma once

#include "epycbench/core/id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::kernel {

enum class ReductionAggregation : std::uint8_t { flat, topology_tree };
inline constexpr std::string_view collective_sum_topology_tree_execution_profile =
    "epycbench.task.sum-u64.partitioned-topology-tree/v1";
inline constexpr std::string_view collective_sum_aggregation_execution_profile =
    "epycbench.task.sum-u64.partitioned-aggregation/v1";

// The position in the supplied span is the canonical rank. NUMA denotes CPU
// locality, independently of the memory policy for the rank's input.
struct ReductionRankLocality {
    core::PackageId package;
    std::optional<core::NumaNodeId> numa{};
    std::optional<core::CacheId> llc{};
};

enum class ReductionLevelKind : std::uint8_t { llc, numa, package, cohort };

struct ReductionGroup {
    // Strictly rank-ordered immediate children. At the LLC level these are
    // ranks from the original cohort. Later levels use prior group leaders.
    std::vector<std::size_t> children;

    [[nodiscard]] auto leader() const noexcept -> std::size_t { return children.front(); }
    auto operator==(const ReductionGroup&) const -> bool = default;
};

struct ReductionLevel {
    ReductionLevelKind kind;
    std::vector<ReductionGroup> groups;
    auto operator==(const ReductionLevel&) const -> bool = default;
};

enum class ReductionTreeError : std::uint8_t {
    empty_cohort, rank_limit_exceeded, allocation_failure, invalid_levels
};

class ReductionTreePlan {
  public:
    // Each level partitions the preceding level's leaders. Groups are ordered
    // by leader rank, and a group with one child carries that rank through.
    [[nodiscard]] static auto create(std::span<const ReductionRankLocality> ranks,
                                     std::size_t maximum_ranks) noexcept
        -> std::expected<ReductionTreePlan, ReductionTreeError>;
    // Check the canonical shape of a retained plan before a caller compares
    // it with a tree derived from the admitted topology and ranks.
    [[nodiscard]] static auto from_levels(std::size_t workers, std::array<ReductionLevel, 4> levels,
                                          std::size_t maximum_ranks) noexcept
        -> std::expected<ReductionTreePlan, ReductionTreeError>;

    [[nodiscard]] auto worker_count() const noexcept -> std::size_t { return workers_; }
    [[nodiscard]] auto leader() const noexcept -> std::size_t { return 0U; }
    [[nodiscard]] auto levels() const noexcept -> std::span<const ReductionLevel, 4> {
        return levels_;
    }
    auto operator==(const ReductionTreePlan&) const -> bool = default;

  private:
    ReductionTreePlan(std::size_t workers, std::array<ReductionLevel, 4> levels) noexcept
        : workers_{workers}, levels_{std::move(levels)} {}

    std::size_t workers_;
    std::array<ReductionLevel, 4> levels_;
};

} // namespace epycbench::kernel
