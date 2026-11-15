#include "epycbench/monitoring/context_switches.hpp"

#include "context_switch_classifier_access.hpp"
#include "context_switch_classifier_kernel.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace epycbench::monitoring {
namespace {
auto context_error(std::string_view code, std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Context-switch classification input is invalid",
            .operation = "classify_context_switches",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}
auto production_drain(void* context) noexcept
    -> std::expected<platform_linux::PerfContextSwitchDrain, core::Error> {
    return static_cast<platform_linux::PerfContextSwitchSource*>(context)->drain();
}
} // namespace

auto detail::checked_context_switch_increment(std::uint64_t value)
    -> std::expected<std::uint64_t, core::Error> {
    if (value == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected{
            context_error("monitoring.context_switches.count_overflow", "event_count")};
    return value + 1U;
}

ContextSwitchEventBatch::ContextSwitchEventBatch(core::WorkerId worker_id,
                                                 ContextSwitchCaptureCompleteness completeness,
                                                 std::vector<ContextSwitchEvent> events) noexcept
    : worker_id_{worker_id}, completeness_{completeness}, events_{std::move(events)} {}
auto ContextSwitchEventBatch::create(core::WorkerId worker_id,
                                     ContextSwitchCaptureCompleteness completeness,
                                     std::vector<ContextSwitchEvent> events)
    -> ContextSwitchEventBatch {
    return {worker_id, completeness, std::move(events)};
}
auto ContextSwitchEventBatch::worker_id() const noexcept -> core::WorkerId { return worker_id_; }
auto ContextSwitchEventBatch::completeness() const noexcept -> ContextSwitchCaptureCompleteness {
    return completeness_;
}
auto ContextSwitchEventBatch::events() const noexcept -> std::span<const ContextSwitchEvent> {
    return events_;
}

ContextSwitchEventSource::ContextSwitchEventSource(
    core::WorkerId worker_id, void* context, DrainOperation drain,
    std::unique_ptr<platform_linux::PerfContextSwitchSource> source) noexcept
    : worker_id_{worker_id}, context_{context}, drain_{drain}, source_{std::move(source)} {}
auto ContextSwitchEventSource::prepare(core::WorkerId worker_id,
                                       platform_linux::LinuxThreadId thread_id)
    -> std::expected<ContextSwitchEventSource, core::Error> {
    auto prepared = platform_linux::PerfContextSwitchSource::prepare(thread_id);
    if (!prepared)
        return std::unexpected{std::move(prepared.error())};
    auto source = std::make_unique<platform_linux::PerfContextSwitchSource>(std::move(*prepared));
    auto* context = source.get();
    return ContextSwitchEventSource{worker_id, context, production_drain, std::move(source)};
}
ContextSwitchEventSource::ContextSwitchEventSource(ContextSwitchEventSource&& other) noexcept
    : worker_id_{other.worker_id_}, context_{other.context_}, drain_{other.drain_},
      source_{std::move(other.source_)} {
    if (source_)
        context_ = source_.get();
    other.context_ = nullptr;
    other.drain_ = nullptr;
}
ContextSwitchEventSource::~ContextSwitchEventSource() noexcept = default;
auto ContextSwitchEventSource::support_callback(void*) noexcept -> MonitoringSourceSupport {
    return MonitoringSourceSupport::supported;
}
auto ContextSwitchEventSource::drain_callback(void* context) noexcept
    -> std::expected<MonitoringPayloadResult<ContextSwitchEventBatch>, core::Error> {
    auto& source = *static_cast<ContextSwitchEventSource*>(context);
    auto drained = source.drain_(source.context_);
    if (!drained)
        return std::unexpected{std::move(drained.error())};
    std::vector<ContextSwitchEvent> events;
    events.reserve(drained->records().size());
    for (const auto& record : drained->records()) {
        if (record.kind() == platform_linux::PerfContextSwitchKind::switch_in)
            continue;
        events.push_back(ContextSwitchEvent::create(
            source.worker_id_, record.timestamp(),
            record.kind() == platform_linux::PerfContextSwitchKind::involuntary_switch_out
                ? ContextSwitchKind::involuntary
                : ContextSwitchKind::voluntary));
    }
    const auto completeness =
        drained->completeness() == platform_linux::PerfCaptureCompleteness::complete
            ? ContextSwitchCaptureCompleteness::complete
            : ContextSwitchCaptureCompleteness::incomplete;
    return MonitoringPayloadResult<ContextSwitchEventBatch>::available(
        ContextSwitchEventBatch::create(source.worker_id_, completeness, std::move(events)));
}
auto ContextSwitchEventSource::operations() noexcept
    -> std::expected<MonitoringSourceOperations<ContextSwitchEventBatch>, core::Error> {
    return MonitoringSourceOperations<ContextSwitchEventBatch>::create(this, support_callback,
                                                                       drain_callback);
}
auto ContextSwitchEventSource::implementation_id() -> MonitoringImplementationId {
    return MonitoringImplementationId::create("linux-perf-context-switch-records");
}

ContextSwitchWorkerAcquisitionView::ContextSwitchWorkerAcquisitionView(
    core::WorkerId worker_id, MonitoringImplementationId implementation_id,
    platform_linux::RawMonotonicTimestamp coverage_start,
    platform_linux::RawMonotonicTimestamp coverage_end,
    ContextSwitchCaptureCompleteness completeness,
    std::span<const Acquisition> acquisitions) noexcept
    : worker_id_{worker_id}, implementation_id_{std::move(implementation_id)},
      coverage_start_{coverage_start}, coverage_end_{coverage_end}, completeness_{completeness},
      acquisitions_{acquisitions} {}
auto ContextSwitchWorkerAcquisitionView::worker_id() const noexcept -> core::WorkerId {
    return worker_id_;
}
auto ContextSwitchWorkerAcquisitionView::implementation_id() const noexcept
    -> const MonitoringImplementationId& {
    return implementation_id_;
}
auto ContextSwitchWorkerAcquisitionView::coverage_start() const noexcept
    -> platform_linux::RawMonotonicTimestamp {
    return coverage_start_;
}
auto ContextSwitchWorkerAcquisitionView::coverage_end() const noexcept
    -> platform_linux::RawMonotonicTimestamp {
    return coverage_end_;
}
auto ContextSwitchWorkerAcquisitionView::completeness() const noexcept
    -> ContextSwitchCaptureCompleteness {
    return completeness_;
}
auto ContextSwitchWorkerAcquisitionView::acquisitions() const noexcept
    -> std::span<const Acquisition> {
    return acquisitions_;
}

WorkerContextSwitchEvidence::WorkerContextSwitchEvidence(
    core::WorkerId worker_id, MonitoringSignalState state,
    std::optional<ContextSwitchCount> voluntary,
    std::optional<ContextSwitchCount> involuntary) noexcept
    : worker_id_{worker_id}, state_{state}, voluntary_{voluntary}, involuntary_{involuntary} {}
auto WorkerContextSwitchEvidence::worker_id() const noexcept -> core::WorkerId {
    return worker_id_;
}
auto WorkerContextSwitchEvidence::state() const noexcept -> MonitoringSignalState { return state_; }
auto WorkerContextSwitchEvidence::voluntary_count() const noexcept
    -> const std::optional<ContextSwitchCount>& {
    return voluntary_;
}
auto WorkerContextSwitchEvidence::involuntary_count() const noexcept
    -> const std::optional<ContextSwitchCount>& {
    return involuntary_;
}
ContextSwitchObservationEvidence::ContextSwitchObservationEvidence(
    MonitoringSignalState state, std::vector<WorkerContextSwitchEvidence> workers) noexcept
    : state_{state}, workers_{std::move(workers)} {}
auto ContextSwitchObservationEvidence::state() const noexcept -> MonitoringSignalState {
    return state_;
}
auto ContextSwitchObservationEvidence::workers() const noexcept
    -> std::span<const WorkerContextSwitchEvidence> {
    return workers_;
}

auto classify_context_switches(platform_linux::RawMonotonicTimestamp observation_start,
                               platform_linux::RawMonotonicTimestamp observation_end,
                               std::span<const core::WorkerId> canonical_workers,
                               std::span<const ContextSwitchWorkerAcquisitionView> views)
    -> std::expected<ContextSwitchObservationEvidence, core::Error> {
    std::vector<WorkerContextSwitchEvidence> evidence;
    evidence.reserve(canonical_workers.size());
    const detail::ContextSwitchEvidenceSink sink{
        &evidence,
        +[](void* raw, core::WorkerId worker, MonitoringSignalState state,
            std::optional<ContextSwitchCount> voluntary,
            std::optional<ContextSwitchCount> involuntary) noexcept {
            static_cast<std::vector<WorkerContextSwitchEvidence>*>(raw)->push_back(
                WorkerContextSwitchEvidence{worker, state, voluntary, involuntary});
            return true;
        }};
    auto classified = detail::classify_context_switches_bounded(
        observation_start, observation_end, canonical_workers, views, sink);
    if (!classified) {
        using Failure = detail::ContextSwitchClassifierFailure;
        const auto [code, reason] = [&]() {
            switch (classified.error()) {
            case Failure::empty_workers:
                return std::pair{"monitoring.context_switches.invalid_input",
                                 "empty_workers"};
            case Failure::duplicate_worker:
                return std::pair{"monitoring.context_switches.invalid_input",
                                 "duplicate_worker"};
            case Failure::reversed_observation_interval:
                return std::pair{"monitoring.context_switches.invalid_input",
                                 "reversed_observation_interval"};
            case Failure::worker_view_count:
                return std::pair{"monitoring.context_switches.association_mismatch",
                                 "worker_view_count"};
            case Failure::worker_source:
                return std::pair{"monitoring.context_switches.association_mismatch",
                                 "worker_source"};
            case Failure::acquisition_metadata:
                return std::pair{"monitoring.context_switches.association_mismatch",
                                 "acquisition_metadata"};
            case Failure::batch_worker:
                return std::pair{"monitoring.context_switches.association_mismatch",
                                 "batch_worker"};
            case Failure::event_worker:
                return std::pair{"monitoring.context_switches.association_mismatch",
                                 "event_worker"};
            case Failure::interval_coverage:
                return std::pair{"monitoring.context_switches.capture_incomplete",
                                 "interval_coverage"};
            case Failure::lost_records:
                return std::pair{"monitoring.context_switches.capture_incomplete",
                                 "lost_records"};
            case Failure::event_count:
                return std::pair{"monitoring.context_switches.count_overflow",
                                 "event_count"};
            }
            std::terminate();
        }();
        return std::unexpected{context_error(code, reason)};
    }
    return ContextSwitchObservationEvidence{*classified, std::move(evidence)};
}

auto detail::classify_context_switches_bounded(
    platform_linux::RawMonotonicTimestamp observation_start,
    platform_linux::RawMonotonicTimestamp observation_end,
    std::span<const core::WorkerId> canonical_workers,
    std::span<const ContextSwitchWorkerAcquisitionView> views,
    ContextSwitchEvidenceSink sink) noexcept
    -> std::expected<MonitoringSignalState, ContextSwitchClassifierFailure> {
    if (observation_end < observation_start)
        return std::unexpected{
            ContextSwitchClassifierFailure::reversed_observation_interval};
    if (canonical_workers.empty())
        return std::unexpected{ContextSwitchClassifierFailure::empty_workers};
    if (views.size() != canonical_workers.size())
        return std::unexpected{
            ContextSwitchClassifierFailure::worker_view_count};
    auto aggregate = MonitoringSignalState::clear;
    for (std::size_t worker_index{}; worker_index < canonical_workers.size();
         ++worker_index) {
        const auto worker = canonical_workers[worker_index];
        for (std::size_t prior{}; prior < worker_index; ++prior)
            if (canonical_workers[prior] == worker)
                return std::unexpected{
                    ContextSwitchClassifierFailure::duplicate_worker};
        const ContextSwitchWorkerAcquisitionView* view{};
        for (const auto& candidate : views) {
            if (candidate.worker_id() == worker) {
                if (view != nullptr)
                    return std::unexpected{
                        ContextSwitchClassifierFailure::worker_source};
                view = &candidate;
            }
        }
        if (view == nullptr ||
            view->implementation_id().value() !=
                "linux-perf-context-switch-records")
            return std::unexpected{
                ContextSwitchClassifierFailure::worker_source};
        if (view->completeness() == ContextSwitchCaptureCompleteness::incomplete ||
            view->coverage_start() > observation_start ||
            view->coverage_end() < observation_end)
            return std::unexpected{
                ContextSwitchClassifierFailure::interval_coverage};
        std::uint64_t voluntary{};
        std::uint64_t involuntary{};
        bool unavailable{};
        for (const auto& acquisition : view->acquisitions()) {
            if (acquisition.channel() != MonitoringChannel::context_switches ||
                acquisition.implementation_id() != view->implementation_id())
                return std::unexpected{
                    ContextSwitchClassifierFailure::acquisition_metadata};
            if (acquisition.status() == MonitoringAcquisitionStatus::unavailable) {
                unavailable = true;
                continue;
            }
            const auto& batch = *acquisition.payload();
            if (batch.worker_id() != worker)
                return std::unexpected{
                    ContextSwitchClassifierFailure::batch_worker};
            if (batch.completeness() == ContextSwitchCaptureCompleteness::incomplete)
                return std::unexpected{
                    ContextSwitchClassifierFailure::lost_records};
            for (const auto& event : batch.events()) {
                if (event.worker_id() != worker)
                    return std::unexpected{
                        ContextSwitchClassifierFailure::event_worker};
                if (event.timestamp() < observation_start ||
                    event.timestamp() > observation_end)
                    continue;
                auto& count = event.kind() == ContextSwitchKind::voluntary
                    ? voluntary
                    : involuntary;
                if (count == std::numeric_limits<std::uint64_t>::max())
                    return std::unexpected{
                        ContextSwitchClassifierFailure::event_count};
                ++count;
            }
        }
        const auto worker_state = unavailable
            ? MonitoringSignalState::unavailable
            : involuntary == 0U ? MonitoringSignalState::clear
                                : MonitoringSignalState::detected;
        if (!sink.append(
                sink.context, worker, worker_state,
                unavailable ? std::nullopt
                            : std::optional{ContextSwitchCount::from_events(voluntary)},
                unavailable ? std::nullopt
                            : std::optional{ContextSwitchCount::from_events(involuntary)}))
            std::terminate();
        if (worker_state == MonitoringSignalState::detected)
            aggregate = MonitoringSignalState::detected;
        else if (worker_state == MonitoringSignalState::unavailable &&
                 aggregate != MonitoringSignalState::detected)
            aggregate = MonitoringSignalState::unavailable;
    }
    return aggregate;
}

} // namespace epycbench::monitoring
