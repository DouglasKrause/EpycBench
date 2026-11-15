#pragma once

#include "epycbench/runtime/ipc/payloads.hpp"

namespace epycbench::runtime {

class RunnerContext;

[[nodiscard]] auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                                             const scenario::MatrixCellId& cell_id) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;

[[nodiscard]] auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                                             const scenario::ComparisonVariantId& variant_id) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;

// Close selected service cores over the context's effective SMT membership.
[[nodiscard]] auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                                             const scenario::MatrixCellId& cell_id,
                                             const RunnerContext& context) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;

[[nodiscard]] auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                                             const scenario::ComparisonVariantId& variant_id,
                                             const RunnerContext& context) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;

} // namespace epycbench::runtime
