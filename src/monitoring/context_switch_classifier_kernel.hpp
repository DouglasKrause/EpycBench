#pragma once

#include "epycbench/monitoring/context_switches.hpp"

#include <expected>
#include <span>

namespace epycbench::monitoring::detail {

enum class ContextSwitchClassifierFailure : unsigned char {
    empty_workers,
    duplicate_worker,
    reversed_observation_interval,
    worker_view_count,
    worker_source,
    acquisition_metadata,
    batch_worker,
    event_worker,
    interval_coverage,
    lost_records,
    event_count,
};

struct ContextSwitchEvidenceSink final {
    void* context{};
    bool (*append)(void*, core::WorkerId, MonitoringSignalState,
                   std::optional<ContextSwitchCount>,
                   std::optional<ContextSwitchCount>) noexcept{};
};

[[nodiscard]] auto classify_context_switches_bounded(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    std::span<const core::WorkerId> canonical_workers,
    std::span<const ContextSwitchWorkerAcquisitionView> views,
    ContextSwitchEvidenceSink sink) noexcept
    -> std::expected<MonitoringSignalState, ContextSwitchClassifierFailure>;

} // namespace epycbench::monitoring::detail
