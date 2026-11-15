#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/kernel/configuration.hpp"
#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/descriptor.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#include "epycbench/measurement/empty_harness.hpp"
#include "epycbench/measurement/observation.hpp"

#include <expected>
#include <span>
#include <vector>

namespace epycbench::measurement {

namespace detail {

struct SequentialReadBandwidthObservationExecutionOperations {
    void* context;
    std::expected<void, core::Error> (*await_ready)(
        void*, std::span<const core::WorkerId>) noexcept;
    void (*cancel_ready)(void*) noexcept;
    void (*release)(void*) noexcept;
    std::expected<void, core::Error> (*await_timed_completion)(void*) noexcept;
    std::expected<std::vector<SequentialReadBandwidthWorkerObservation>, core::Error>
        (*collect_worker_evidence)(void*) noexcept;
};

struct DependentLoadLatencyObservationExecutionOperations {
    void* context;
    std::expected<void, core::Error> (*await_ready)(
        void*, std::span<const core::WorkerId>) noexcept;
    void (*cancel_ready)(void*) noexcept;
    void (*release)(void*) noexcept;
    std::expected<void, core::Error> (*await_timed_completion)(void*) noexcept;
    std::expected<std::vector<DependentLoadLatencyWorkerObservation>, core::Error>
        (*collect_worker_evidence)(void*) noexcept;
};

} // namespace detail

[[nodiscard]] auto execute_sequential_read_bandwidth_observation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::SequentialReadBandwidthConfig configuration,
    kernel::SequentialReadBandwidthPassCount pass_count,
    std::span<const core::WorkerId> expected_workers,
    const detail::RawMonotonicClockOperations& clock,
    const detail::SequentialReadBandwidthObservationExecutionOperations& operations)
    -> std::expected<SequentialReadBandwidthObservation, core::Error>;

[[nodiscard]] auto execute_dependent_load_latency_observation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::DependentLoadLatencyConfig configuration,
    std::span<const core::WorkerId> expected_workers,
    const detail::RawMonotonicClockOperations& clock,
    const detail::DependentLoadLatencyObservationExecutionOperations& operations)
    -> std::expected<DependentLoadLatencyObservation, core::Error>;

} // namespace epycbench::measurement
