#pragma once

#include "scenario/retained_plan_projection.hpp"

namespace epycbench::scenario::detail {
struct ProjectionResourceUsage {
    std::size_t nodes{}, objects{}, arrays{}, object_members{}, string_values{}, key_bytes{},
        string_bytes{}, depth{}, canonical_bytes{}, utf32_reserved_elements{}, utf16_elements{},
        canonical_member_elements{}, quoted_key_bytes{}, path_bytes{}, index_decimal_bytes{},
        ordered_staging_bytes{}, unordered_clone_bytes{}, projector_auxiliary_bytes{}, fixed_bytes{},
        error_bytes{}, simultaneous_bytes{}, work_units{};
    auto operator==(const ProjectionResourceUsage&) const -> bool = default;
};
using ProjectionResourceLimits = ProjectionResourceUsage;
struct ProjectionResources {
    ProjectionResourceUsage scenario, resolved;
    auto operator==(const ProjectionResources&) const -> bool = default;
};
enum class ProjectionResourceFailure { invalid_limits, capacity, malformed_input };
[[nodiscard]] auto measure_retained_projection_resources(const RetainedPlanProjectionView&,
    ProjectionResourceLimits scenario, ProjectionResourceLimits resolved)
    -> std::expected<ProjectionResources, ProjectionResourceFailure>;
namespace projection_resource_detail {
[[nodiscard]] auto valid_limits(const ProjectionResourceLimits&) noexcept -> bool;
[[nodiscard]] auto walk_fixed_stock() noexcept -> std::size_t;
[[nodiscard]] auto ordered_member_reservation(std::size_t width, std::size_t maximum) noexcept
    -> std::optional<std::size_t>;
[[nodiscard]] auto checked_add(std::size_t&, std::size_t) noexcept -> bool;
[[nodiscard]] auto checked_multiply(std::size_t, std::size_t) noexcept -> std::optional<std::size_t>;
}
} // namespace epycbench::scenario::detail
