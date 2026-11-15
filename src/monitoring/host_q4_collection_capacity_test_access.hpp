#pragma once

#include "monitoring/host_q4_collection_capacity.hpp"

namespace epycbench::monitoring::qualification::detail {
// Deliberately declared only in this test-access header; production callers cannot select a
// synthetic unsupported branch.
[[nodiscard]] auto
monitoring_q4_unsupported_candidate_test(const MonitoringQ4CollectionCapacityInputs&, std::uint64_t,
                                         MonitoringQ4UnsupportedReason) noexcept
    -> std::expected<MonitoringQ4CapacityAudit, MonitoringQ4CapacityFailure>;
} // namespace epycbench::monitoring::qualification::detail
