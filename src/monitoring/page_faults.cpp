#include "epycbench/monitoring/page_faults.hpp"

#include "page_fault_classifier_kernel.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace epycbench::monitoring {
namespace {

[[nodiscard]] auto page_fault_error(std::string_view code, std::string_view classification)
    -> core::Error {
    return core::Error{
        .category = core::ErrorCategory::monitoring,
        .code = core::StableErrorCode{std::string{code}},
        .message = "Page-fault classification input is invalid",
        .operation = "classify_page_faults",
        .os_error = std::nullopt,
        .context = {{"classification", classification}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_experiment,
    };
}

[[nodiscard]] auto production_read(void*, platform_linux::LinuxThreadId thread_id) noexcept
    -> std::expected<platform_linux::LinuxTaskPageFaultCounters, core::Error> {
    return platform_linux::read_task_page_fault_counters(thread_id);
}

} // namespace

PageFaultSource::PageFaultSource(core::WorkerId worker_id, platform_linux::LinuxThreadId thread_id,
                                 void* reader_context, ReadOperation read_operation) noexcept
    : worker_id_{worker_id}, thread_id_{thread_id}, reader_context_{reader_context},
      read_operation_{read_operation} {}

auto PageFaultSource::create(core::WorkerId worker_id,
                             platform_linux::LinuxThreadId thread_id) noexcept -> PageFaultSource {
    return PageFaultSource{worker_id, thread_id, nullptr, production_read};
}

auto PageFaultSource::support_callback(void*) noexcept -> MonitoringSourceSupport {
    return MonitoringSourceSupport::supported;
}

auto PageFaultSource::read_callback(void* context) noexcept
    -> std::expected<MonitoringPayloadResult<PageFaultSnapshot>, core::Error> {
    auto& source = *static_cast<PageFaultSource*>(context);
    auto counters = source.read_operation_(source.reader_context_, source.thread_id_);
    if (!counters) {
        return std::unexpected{std::move(counters.error())};
    }
    return MonitoringPayloadResult<PageFaultSnapshot>::available(PageFaultSnapshot::create(
        source.worker_id_, PageFaultCount::from_events(counters->minor_faults()),
        PageFaultCount::from_events(counters->major_faults())));
}

auto PageFaultSource::operations() noexcept
    -> std::expected<MonitoringSourceOperations<PageFaultSnapshot>, core::Error> {
    return MonitoringSourceOperations<PageFaultSnapshot>::create(this, support_callback,
                                                                 read_callback);
}

auto PageFaultSource::implementation_id() -> MonitoringImplementationId {
    return MonitoringImplementationId::create("linux-proc-task-stat");
}

WorkerPageFaultEvidence::WorkerPageFaultEvidence(core::WorkerId worker_id,
                                                 MonitoringSignalState state,
                                                 std::optional<PageFaultCount> minor_delta,
                                                 std::optional<PageFaultCount> major_delta) noexcept
    : worker_id_{worker_id}, state_{state}, minor_delta_{minor_delta}, major_delta_{major_delta} {}

auto WorkerPageFaultEvidence::worker_id() const noexcept -> core::WorkerId { return worker_id_; }
auto WorkerPageFaultEvidence::state() const noexcept -> MonitoringSignalState { return state_; }
auto WorkerPageFaultEvidence::minor_delta() const noexcept -> const std::optional<PageFaultCount>& {
    return minor_delta_;
}
auto WorkerPageFaultEvidence::major_delta() const noexcept -> const std::optional<PageFaultCount>& {
    return major_delta_;
}

PageFaultObservationEvidence::PageFaultObservationEvidence(
    MonitoringSignalState state, std::vector<WorkerPageFaultEvidence> workers) noexcept
    : state_{state}, workers_{std::move(workers)} {}

auto PageFaultObservationEvidence::state() const noexcept -> MonitoringSignalState {
    return state_;
}
auto PageFaultObservationEvidence::workers() const noexcept
    -> std::span<const WorkerPageFaultEvidence> {
    return workers_;
}

auto detail::classify_page_faults_bounded(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    std::span<const PageFaultAcquisitionPair> workers,
    PageFaultEvidenceSink sink) noexcept
    -> std::expected<MonitoringSignalState, PageFaultClassifierFailure> {
    if (observation_end < observation_start) {
        return std::unexpected{
            PageFaultClassifierFailure::reversed_observation_interval};
    }
    if (workers.empty()) {
        return std::unexpected{PageFaultClassifierFailure::empty_workers};
    }

    auto state = MonitoringSignalState::clear;
    for (std::size_t index{}; index < workers.size(); ++index) {
        const auto& pair = workers[index];
        for (std::size_t prior{}; prior < index; ++prior)
            if (workers[prior].worker_id() == pair.worker_id())
                return std::unexpected{
                    PageFaultClassifierFailure::duplicate_worker};
        const auto& before = pair.before();
        const auto& after = pair.after();
        if (before.channel() != MonitoringChannel::page_faults ||
            after.channel() != MonitoringChannel::page_faults ||
            before.phase() != MonitoringSamplePhase::before_observation ||
            after.phase() != MonitoringSamplePhase::after_observation ||
            before.implementation_id() != after.implementation_id()) {
            return std::unexpected{
                PageFaultClassifierFailure::acquisition_metadata};
        }
        if (before.collection_end() > observation_start) {
            return std::unexpected{
                PageFaultClassifierFailure::before_overlaps_observation};
        }
        if (after.collection_start() < observation_end) {
            return std::unexpected{
                PageFaultClassifierFailure::after_overlaps_observation};
        }

        if (before.status() == MonitoringAcquisitionStatus::unavailable ||
            after.status() == MonitoringAcquisitionStatus::unavailable) {
            if (!sink.append(sink.context, pair.worker_id(),
                             MonitoringSignalState::unavailable, std::nullopt,
                             std::nullopt))
                std::terminate();
            if (state != MonitoringSignalState::detected)
                state = MonitoringSignalState::unavailable;
            continue;
        }

        const auto& before_snapshot = *before.payload();
        const auto& after_snapshot = *after.payload();
        if (before_snapshot.worker_id() != pair.worker_id() ||
            after_snapshot.worker_id() != pair.worker_id() ||
            before_snapshot.worker_id() != after_snapshot.worker_id()) {
            return std::unexpected{PageFaultClassifierFailure::worker_scope};
        }
        if (after_snapshot.minor_faults() < before_snapshot.minor_faults() ||
            after_snapshot.major_faults() < before_snapshot.major_faults()) {
            return std::unexpected{
                PageFaultClassifierFailure::cumulative_counter_decreased};
        }

        const auto minor_delta = PageFaultCount::from_events(
            after_snapshot.minor_faults().value() - before_snapshot.minor_faults().value());
        const auto major_delta = PageFaultCount::from_events(
            after_snapshot.major_faults().value() - before_snapshot.major_faults().value());
        const auto worker_state = major_delta.value() == 0U
            ? MonitoringSignalState::clear
            : MonitoringSignalState::detected;
        if (!sink.append(sink.context, pair.worker_id(), worker_state,
                         minor_delta, major_delta))
            std::terminate();
        if (worker_state == MonitoringSignalState::detected)
            state = MonitoringSignalState::detected;
    }
    return state;
}

auto classify_page_faults(platform_linux::RawMonotonicTimestamp observation_start,
                          platform_linux::RawMonotonicTimestamp observation_end,
                          std::span<const PageFaultAcquisitionPair> workers)
    -> std::expected<PageFaultObservationEvidence, core::Error> {
    std::vector<WorkerPageFaultEvidence> evidence;
    evidence.reserve(workers.size());
    const detail::PageFaultEvidenceSink sink{
        &evidence,
        +[](void* raw, core::WorkerId worker, MonitoringSignalState state,
            std::optional<PageFaultCount> minor,
            std::optional<PageFaultCount> major) noexcept {
            static_cast<std::vector<WorkerPageFaultEvidence>*>(raw)->push_back(
                WorkerPageFaultEvidence{worker, state, minor, major});
            return true;
        }};
    auto result = detail::classify_page_faults_bounded(
        observation_start, observation_end, workers, sink);
    if (result)
        return PageFaultObservationEvidence{*result, std::move(evidence)};
    switch (result.error()) {
    case detail::PageFaultClassifierFailure::empty_workers:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.invalid_input", "empty_workers")};
    case detail::PageFaultClassifierFailure::duplicate_worker:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.invalid_input", "duplicate_worker")};
    case detail::PageFaultClassifierFailure::worker_scope:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.association_mismatch", "worker_scope")};
    case detail::PageFaultClassifierFailure::acquisition_metadata:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.association_mismatch",
            "acquisition_metadata")};
    case detail::PageFaultClassifierFailure::reversed_observation_interval:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.invalid_input",
            "reversed_observation_interval")};
    case detail::PageFaultClassifierFailure::before_overlaps_observation:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.invalid_bracket",
            "before_overlaps_observation")};
    case detail::PageFaultClassifierFailure::after_overlaps_observation:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.invalid_bracket",
            "after_overlaps_observation")};
    case detail::PageFaultClassifierFailure::cumulative_counter_decreased:
        return std::unexpected{page_fault_error(
            "monitoring.page_faults.counter_decreased",
            "cumulative_counter_decreased")};
    }
    std::terminate();
}

} // namespace epycbench::monitoring
