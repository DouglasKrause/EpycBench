#include "epycbench/measurement/observation_execution.hpp"

#include <string>
#include <utility>

namespace epycbench::measurement {
namespace {

[[nodiscard]] auto execution_error(std::string code, std::string operation,
                                   std::string classification) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{std::move(code)},
        .message = "Measurement observation execution failed",
        .operation = std::move(operation),
        .os_error = std::nullopt,
        .context = {{"classification", std::move(classification)}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

[[nodiscard]] auto empty_workers_error(std::string operation) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{"measurement.observation.empty_workers"},
        .message = "Measurement observation construction failed",
        .operation = std::move(operation),
        .os_error = std::nullopt,
        .context = {{"classification", "zero_completed_workers"}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

[[nodiscard]] auto worker_evidence_mismatch_error(std::string operation) -> core::Error {
    return execution_error("measurement.observation_execution.worker_evidence_mismatch",
                           std::move(operation), "worker_evidence_mismatch");
}

template <typename WorkerObservation>
[[nodiscard]] auto worker_evidence_matches(
    std::span<const core::WorkerId> expected_workers,
    const std::vector<WorkerObservation>& workers) noexcept -> bool {
    if (workers.size() != expected_workers.size()) {
        return false;
    }
    for (std::size_t index = 0; index < workers.size(); ++index) {
        if (workers[index].worker_id() != expected_workers[index]) {
            return false;
        }
    }
    return true;
}

template <typename Operations>
[[nodiscard]] auto acquire_enclosing_interval(
    std::span<const core::WorkerId> expected_workers,
    const detail::RawMonotonicClockOperations& clock, const Operations& operations)
    -> std::expected<MeasurementInterval, core::Error> {
    auto ready = operations.await_ready(operations.context, expected_workers);
    if (!ready) {
        return std::unexpected{std::move(ready.error())};
    }

    auto start = clock.now(clock.context);
    if (!start) {
        operations.cancel_ready(operations.context);
        return std::unexpected{std::move(start.error())};
    }

    operations.release(operations.context);

    auto completed = operations.await_timed_completion(operations.context);
    if (!completed) {
        return std::unexpected{std::move(completed.error())};
    }

    auto end = clock.now(clock.context);
    if (!end) {
        return std::unexpected{std::move(end.error())};
    }

    return MeasurementInterval::create(*start, *end);
}

} // namespace

auto execute_sequential_read_bandwidth_observation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::SequentialReadBandwidthConfig configuration,
    kernel::SequentialReadBandwidthPassCount pass_count,
    std::span<const core::WorkerId> expected_workers,
    const detail::RawMonotonicClockOperations& clock,
    const detail::SequentialReadBandwidthObservationExecutionOperations& operations)
    -> std::expected<SequentialReadBandwidthObservation, core::Error> {
    constexpr auto operation = "measurement.observation_execution.bandwidth.execute";
    if (expected_workers.empty()) {
        return std::unexpected{empty_workers_error(operation)};
    }

    auto enclosing = acquire_enclosing_interval(expected_workers, clock, operations);
    if (!enclosing) {
        return std::unexpected{std::move(enclosing.error())};
    }

    auto workers = operations.collect_worker_evidence(operations.context);
    if (!workers) {
        return std::unexpected{std::move(workers.error())};
    }
    if (!worker_evidence_matches(expected_workers, *workers)) {
        return std::unexpected{worker_evidence_mismatch_error(operation)};
    }

    return SequentialReadBandwidthObservation::create(
        observation_id, experiment_id, block_id, implementation_id, configuration, pass_count,
        std::move(*workers), std::move(*enclosing));
}

auto execute_dependent_load_latency_observation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::DependentLoadLatencyConfig configuration,
    std::span<const core::WorkerId> expected_workers,
    const detail::RawMonotonicClockOperations& clock,
    const detail::DependentLoadLatencyObservationExecutionOperations& operations)
    -> std::expected<DependentLoadLatencyObservation, core::Error> {
    constexpr auto operation = "measurement.observation_execution.latency.execute";
    if (expected_workers.empty()) {
        return std::unexpected{empty_workers_error(operation)};
    }

    auto enclosing = acquire_enclosing_interval(expected_workers, clock, operations);
    if (!enclosing) {
        return std::unexpected{std::move(enclosing.error())};
    }

    auto workers = operations.collect_worker_evidence(operations.context);
    if (!workers) {
        return std::unexpected{std::move(workers.error())};
    }
    if (!worker_evidence_matches(expected_workers, *workers)) {
        return std::unexpected{worker_evidence_mismatch_error(operation)};
    }

    return DependentLoadLatencyObservation::create(
        observation_id, experiment_id, block_id, implementation_id, std::move(configuration),
        std::move(*workers), std::move(*enclosing));
}

} // namespace epycbench::measurement
