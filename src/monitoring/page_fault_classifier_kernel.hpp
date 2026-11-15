#pragma once

#include "epycbench/monitoring/page_faults.hpp"

#include <expected>
#include <span>

namespace epycbench::monitoring::detail {

enum class PageFaultClassifierFailure : unsigned char {
    empty_workers,
    duplicate_worker,
    worker_scope,
    acquisition_metadata,
    reversed_observation_interval,
    before_overlaps_observation,
    after_overlaps_observation,
    cumulative_counter_decreased,
};

struct PageFaultEvidenceSink final {
    void* context{};
    bool (*append)(void*, core::WorkerId, MonitoringSignalState,
                   std::optional<PageFaultCount>,
                   std::optional<PageFaultCount>) noexcept{};
};

[[nodiscard]] auto classify_page_faults_bounded(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    std::span<const PageFaultAcquisitionPair> workers,
    PageFaultEvidenceSink sink) noexcept
    -> std::expected<MonitoringSignalState, PageFaultClassifierFailure>;

} // namespace epycbench::monitoring::detail
