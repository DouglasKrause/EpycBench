#include "epycbench/measurement/observation.hpp"

#include <utility>

namespace epycbench::measurement {
namespace {

[[nodiscard]] auto construction_error(std::string code, std::string operation,
                                      std::string classification) -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::measurement,
        .code = core::StableErrorCode{std::move(code)},
        .message = "Measurement observation construction failed",
        .operation = std::move(operation),
        .os_error = std::nullopt,
        .context = {{"classification", std::move(classification)}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

[[nodiscard]] auto reversed_interval_error() -> core::Error {
    return construction_error("measurement.interval.invalid_order", "measurement.interval.create",
                              "end_precedes_start");
}

[[nodiscard]] auto zero_workers_error(std::string operation) -> core::Error {
    return construction_error("measurement.observation.empty_workers", std::move(operation),
                              "zero_completed_workers");
}

[[nodiscard]] auto aggregate_overflow_error(std::string operation) -> core::Error {
    return construction_error("measurement.observation.aggregate_work_overflow",
                              std::move(operation), "aggregate_work_overflow");
}

[[nodiscard]] auto implementation_mismatch_error(std::string operation) -> core::Error {
    return construction_error("measurement.observation.implementation_mismatch",
                              std::move(operation), "fixed_family_implementation_mismatch");
}

} // namespace

MeasurementInterval::MeasurementInterval(platform_linux::RawMonotonicTimestamp start,
                                         platform_linux::RawMonotonicTimestamp end,
                                         core::Duration elapsed) noexcept
    : start_{start}, end_{end}, elapsed_{elapsed} {}

auto MeasurementInterval::create(platform_linux::RawMonotonicTimestamp start,
                                 platform_linux::RawMonotonicTimestamp end)
    -> std::expected<MeasurementInterval, core::Error> {
    if (end < start) {
        return std::unexpected{reversed_interval_error()};
    }
    return MeasurementInterval{
        start, end, core::Duration::from_base_units(end.nanoseconds() - start.nanoseconds())};
}

auto MeasurementInterval::start() const noexcept -> platform_linux::RawMonotonicTimestamp {
    return start_;
}
auto MeasurementInterval::end() const noexcept -> platform_linux::RawMonotonicTimestamp {
    return end_;
}
auto MeasurementInterval::elapsed() const noexcept -> core::Duration { return elapsed_; }

SequentialReadBandwidthWorkerObservation::SequentialReadBandwidthWorkerObservation(
    core::WorkerId worker_id, MeasurementInterval interval,
    kernel::SequentialReadBandwidthWork work,
    kernel::SequentialReadBandwidthVerification verification) noexcept
    : worker_id_{worker_id}, interval_{std::move(interval)}, work_{work},
      verification_{verification} {}

auto SequentialReadBandwidthWorkerObservation::create(
    core::WorkerId worker_id, MeasurementInterval interval,
    kernel::SequentialReadBandwidthWork work,
    kernel::SequentialReadBandwidthVerification verification) noexcept
    -> SequentialReadBandwidthWorkerObservation {
    return {worker_id, std::move(interval), work, verification};
}
auto SequentialReadBandwidthWorkerObservation::worker_id() const noexcept -> core::WorkerId {
    return worker_id_;
}
auto SequentialReadBandwidthWorkerObservation::interval() const noexcept
    -> const MeasurementInterval& {
    return interval_;
}
auto SequentialReadBandwidthWorkerObservation::work() const noexcept
    -> kernel::SequentialReadBandwidthWork {
    return work_;
}
auto SequentialReadBandwidthWorkerObservation::verification() const noexcept
    -> kernel::SequentialReadBandwidthVerification {
    return verification_;
}

DependentLoadLatencyWorkerObservation::DependentLoadLatencyWorkerObservation(
    core::WorkerId worker_id, MeasurementInterval interval,
    kernel::DependentLoadLatencySchedule schedule,
    kernel::DependentLoadLatencyVerification verification) noexcept
    : worker_id_{worker_id}, interval_{std::move(interval)}, schedule_{schedule},
      verification_{verification} {}

auto DependentLoadLatencyWorkerObservation::create(
    core::WorkerId worker_id, MeasurementInterval interval,
    kernel::DependentLoadLatencySchedule schedule,
    kernel::DependentLoadLatencyVerification verification) noexcept
    -> DependentLoadLatencyWorkerObservation {
    return {worker_id, std::move(interval), schedule, verification};
}
auto DependentLoadLatencyWorkerObservation::worker_id() const noexcept -> core::WorkerId {
    return worker_id_;
}
auto DependentLoadLatencyWorkerObservation::interval() const noexcept
    -> const MeasurementInterval& {
    return interval_;
}
auto DependentLoadLatencyWorkerObservation::schedule() const noexcept
    -> kernel::DependentLoadLatencySchedule {
    return schedule_;
}
auto DependentLoadLatencyWorkerObservation::completed_work() const noexcept
    -> kernel::DependentLoadLatencyStepCount {
    return schedule_.step_count();
}
auto DependentLoadLatencyWorkerObservation::verification() const noexcept
    -> kernel::DependentLoadLatencyVerification {
    return verification_;
}

SequentialReadBandwidthObservation::SequentialReadBandwidthObservation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::SequentialReadBandwidthConfig configuration,
    kernel::SequentialReadBandwidthPassCount pass_count,
    std::vector<SequentialReadBandwidthWorkerObservation> workers,
    MeasurementInterval enclosing_interval, core::ByteCount aggregate_work) noexcept
    : observation_id_{observation_id}, experiment_id_{experiment_id}, block_id_{block_id},
      implementation_id_{implementation_id}, configuration_{configuration},
      pass_count_{pass_count}, workers_{std::move(workers)},
      enclosing_interval_{std::move(enclosing_interval)}, aggregate_work_{aggregate_work} {}

auto SequentialReadBandwidthObservation::create(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::SequentialReadBandwidthConfig configuration,
    kernel::SequentialReadBandwidthPassCount pass_count,
    std::vector<SequentialReadBandwidthWorkerObservation> workers,
    MeasurementInterval enclosing_interval)
    -> std::expected<SequentialReadBandwidthObservation, core::Error> {
    constexpr auto operation = "measurement.observation.bandwidth.create";
    if (implementation_id != kernel::KernelImplementationId::scalar_u64_sequential_v1) {
        return std::unexpected{implementation_mismatch_error(operation)};
    }
    if (workers.empty()) {
        return std::unexpected{zero_workers_error(operation)};
    }
    auto aggregate = core::ByteCount::from_base_units(0U);
    for (const auto& worker : workers) {
        const auto added = aggregate.checked_add(worker.work().logical_bytes_read());
        if (!added) {
            return std::unexpected{aggregate_overflow_error(operation)};
        }
        aggregate = *added;
    }
    return SequentialReadBandwidthObservation{
        observation_id, experiment_id, block_id, implementation_id, configuration, pass_count,
        std::move(workers), std::move(enclosing_interval), aggregate};
}

auto SequentialReadBandwidthObservation::observation_id() const noexcept -> core::ObservationId {
    return observation_id_;
}
auto SequentialReadBandwidthObservation::experiment_id() const noexcept -> core::ExperimentId {
    return experiment_id_;
}
auto SequentialReadBandwidthObservation::block_id() const noexcept
    -> kernel::DependentLoadLatencyBlockId {
    return block_id_;
}
auto SequentialReadBandwidthObservation::implementation_id() const noexcept
    -> kernel::KernelImplementationId {
    return implementation_id_;
}
auto SequentialReadBandwidthObservation::configuration() const noexcept
    -> const kernel::SequentialReadBandwidthConfig& {
    return configuration_;
}
auto SequentialReadBandwidthObservation::pass_count() const noexcept
    -> kernel::SequentialReadBandwidthPassCount {
    return pass_count_;
}
auto SequentialReadBandwidthObservation::workers() const noexcept
    -> std::span<const SequentialReadBandwidthWorkerObservation> {
    return workers_;
}
auto SequentialReadBandwidthObservation::enclosing_interval() const noexcept
    -> const MeasurementInterval& {
    return enclosing_interval_;
}
auto SequentialReadBandwidthObservation::aggregate_work() const noexcept -> core::ByteCount {
    return aggregate_work_;
}

DependentLoadLatencyObservation::DependentLoadLatencyObservation(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::DependentLoadLatencyConfig configuration,
    std::vector<DependentLoadLatencyWorkerObservation> workers,
    MeasurementInterval enclosing_interval, core::WorkCount aggregate_work) noexcept
    : observation_id_{observation_id}, experiment_id_{experiment_id}, block_id_{block_id},
      implementation_id_{implementation_id}, configuration_{std::move(configuration)},
      workers_{std::move(workers)}, enclosing_interval_{std::move(enclosing_interval)},
      aggregate_work_{aggregate_work} {}

auto DependentLoadLatencyObservation::create(
    core::ObservationId observation_id, core::ExperimentId experiment_id,
    kernel::DependentLoadLatencyBlockId block_id,
    kernel::KernelImplementationId implementation_id,
    kernel::DependentLoadLatencyConfig configuration,
    std::vector<DependentLoadLatencyWorkerObservation> workers,
    MeasurementInterval enclosing_interval)
    -> std::expected<DependentLoadLatencyObservation, core::Error> {
    constexpr auto operation = "measurement.observation.latency.create";
    if (implementation_id != kernel::KernelImplementationId::scalar_u64_random_cycle_v1) {
        return std::unexpected{implementation_mismatch_error(operation)};
    }
    if (workers.empty()) {
        return std::unexpected{zero_workers_error(operation)};
    }
    auto aggregate = core::WorkCount::from_base_units(0U);
    for (const auto& worker : workers) {
        const auto work = core::WorkCount::from_base_units(worker.completed_work().value());
        const auto added = aggregate.checked_add(work);
        if (!added) {
            return std::unexpected{aggregate_overflow_error(operation)};
        }
        aggregate = *added;
    }
    return DependentLoadLatencyObservation{
        observation_id, experiment_id, block_id, implementation_id, std::move(configuration),
        std::move(workers), std::move(enclosing_interval), aggregate};
}

auto DependentLoadLatencyObservation::observation_id() const noexcept -> core::ObservationId {
    return observation_id_;
}
auto DependentLoadLatencyObservation::experiment_id() const noexcept -> core::ExperimentId {
    return experiment_id_;
}
auto DependentLoadLatencyObservation::block_id() const noexcept
    -> kernel::DependentLoadLatencyBlockId {
    return block_id_;
}
auto DependentLoadLatencyObservation::implementation_id() const noexcept
    -> kernel::KernelImplementationId {
    return implementation_id_;
}
auto DependentLoadLatencyObservation::configuration() const noexcept
    -> const kernel::DependentLoadLatencyConfig& {
    return configuration_;
}
auto DependentLoadLatencyObservation::workers() const noexcept
    -> std::span<const DependentLoadLatencyWorkerObservation> {
    return workers_;
}
auto DependentLoadLatencyObservation::enclosing_interval() const noexcept
    -> const MeasurementInterval& {
    return enclosing_interval_;
}
auto DependentLoadLatencyObservation::aggregate_work() const noexcept -> core::WorkCount {
    return aggregate_work_;
}

} // namespace epycbench::measurement
