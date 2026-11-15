#pragma once

#include "epycbench/runtime/pairwise_production_authority.hpp"
#include "epycbench/runtime/runner_context.hpp"
#include "epycbench/runtime/statistics_production_policy.hpp"

namespace epycbench::runtime::detail {

struct PairwiseProductionAdmissionAccess final {
    [[nodiscard]] static auto create(
        ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
        ipc::RequestId second_id, std::uint64_t resolved_comparison_order_seed,
        StatisticsComparisonPolicySource statistics_source,
        const RunnerContext& context) noexcept
        -> std::expected<PairwiseProductionAdmission, core::Error>;
};

} // namespace epycbench::runtime::detail
