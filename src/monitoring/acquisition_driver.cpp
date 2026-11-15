#include "epycbench/monitoring/acquisition_driver.hpp"
#include "acquisition_driver_access.hpp"
#include "interruptible_raw_scheduler_kernel.hpp"
#include "../core/json_ownership.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace epycbench::monitoring {
namespace detail {
enum class MonitoringCaptureRegistrationState : std::uint8_t { unregistered, active, retired };
struct MonitoringCaptureRegistration {
    MonitoringCaptureRegistrationState state{MonitoringCaptureRegistrationState::unregistered};
    std::uint64_t generation{};
};
} // namespace detail
namespace {

std::atomic injected_boundary{detail::AcquisitionDriverExceptionBoundary::none};
std::atomic injected_kind{detail::AcquisitionDriverExceptionKind::standard};
void inject_if(detail::AcquisitionDriverExceptionBoundary boundary) {
    auto expected = boundary;
    if (!injected_boundary.compare_exchange_strong(
            expected, detail::AcquisitionDriverExceptionBoundary::none)) return;
    if (injected_kind.load() == detail::AcquisitionDriverExceptionKind::standard)
        throw std::runtime_error{"injected"};
    throw 1;
}

auto driver_error(std::string code, std::string classification) -> core::Error {
    auto context = core::detail::json_object<nlohmann::json>([&](auto& object) {
        nlohmann::json value(std::move(classification));
        core::detail::append_object(object, "classification", value);
    });
    core::detail::JsonCleanup context_cleanup{context};
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Monitoring acquisition driver operation failed",
            .operation = "monitoring_driver",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

// Selection is an ordinary post-disarm operation. It validates the entire
// retained sequence, including facts excluded from the classifier view.
auto point_selection_error(MonitoringChannel channel, bool association,
                           std::string_view classification) -> core::Error {
    const bool temperature = channel == MonitoringChannel::temperature;
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{std::string{temperature
                ? (association ? "monitoring.temperature.association_mismatch"
                               : "monitoring.temperature.invalid_input")
                : (association ? "monitoring.frequency.association_mismatch"
                               : "monitoring.frequency.invalid_input")}},
            .message = temperature ? "Temperature classification input is invalid"
                                   : "Frequency classification input is invalid",
            .operation = temperature ? "classify_temperature" : "classify_frequency",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

template <class Payload, class Association>
auto inside_points(std::span<const TimestampedMonitoringAcquisition<Payload>> acquisitions,
                   platform_linux::RawMonotonicTimestamp start,
                   platform_linux::RawMonotonicTimestamp end,
                   MonitoringChannel channel, Association association)
    -> std::expected<decltype(acquisitions), core::Error> {
    std::size_t first{}, count{};
    std::optional<platform_linux::RawMonotonicTimestamp> previous;
    for (std::size_t position{}; position < acquisitions.size(); ++position) {
        const auto& item = acquisitions[position];
        if (item.collection_end() < item.collection_start())
            return std::unexpected{point_selection_error(channel, false, "interval")};
        if (previous && item.collection_start() < *previous)
            return std::unexpected{point_selection_error(channel, false, "ordering")};
        previous = item.collection_start();
        if (const auto reason = association(item); !reason.empty())
            return std::unexpected{point_selection_error(channel, true, reason)};
        // Keep every raw fact, but classify only wholly contained points.
        // A subspan cannot represent a hole in the eligible sequence.
        if (item.collection_start() >= start && item.collection_end() <= end) {
            if (count == 0U) first = position;
            else if (position - first != count)
                return std::unexpected{point_selection_error(channel, false, "ordering")};
            ++count;
        }
    }
    return acquisitions.subspan(first, count);
}

void retire_source_context(nlohmann::json& root) noexcept {
    // Callback-owned Errors impose no depth bound. Re-scan the exclusively
    // owned tree instead of keeping a parent stack: each pass removes one
    // structured node after every structured descendant has been retired.
    // Only primitive/null children remain when clear destroys a leaf.
    while (root.is_structured()) {
        auto* leaf = &root;
        for (;;) {
            const auto child = std::find_if(leaf->begin(), leaf->end(),
                [](const auto& value) { return value.is_structured(); });
            if (child == leaf->end()) {
                leaf->clear();
                *leaf = nullptr;
                break;
            }
            leaf = &*child;
        }
    }
}

constexpr auto index(MonitoringChannel channel) noexcept -> std::size_t {
    switch (channel) {
    case MonitoringChannel::page_faults: return 0;
    case MonitoringChannel::context_switches: return 1;
    case MonitoringChannel::temperature: return 2;
    case MonitoringChannel::frequency: return 3;
    case MonitoringChannel::throttling: return 4;
    }
    return 4;
}

auto checked_deadline(platform_linux::RawMonotonicTimestamp origin, core::Duration interval,
                      std::uint64_t round)
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    if (round != 0 && interval.value() > std::numeric_limits<std::uint64_t>::max() / round)
        return std::unexpected{driver_error("monitoring.driver.invariant_failed",
                                            "deadline_overflow")};
    const auto delta = interval.value() * round;
    if (delta > std::numeric_limits<std::uint64_t>::max() - origin.nanoseconds())
        return std::unexpected{driver_error("monitoring.driver.invariant_failed",
                                            "deadline_overflow")};
    return platform_linux::RawMonotonicTimestamp::from_nanoseconds(origin.nanoseconds() + delta);
}

template <class Payload> struct ReadProxy {
    void* context;
    std::expected<Payload, core::Error> (*read)(void*) noexcept;
};

template <class Payload>
auto proxy_read(void* opaque) noexcept
    -> std::expected<MonitoringPayloadResult<Payload>, core::Error> {
    auto& proxy = *static_cast<ReadProxy<Payload>*>(opaque);
    auto value = proxy.read(proxy.context);
    if (!value) return std::unexpected{value.error()};
    return MonitoringPayloadResult<Payload>::available(std::move(*value));
}

auto proxy_support(void*) noexcept -> MonitoringSourceSupport {
    return MonitoringSourceSupport::supported;
}

template <class Payload>
auto acquire(MonitoringDriverOperations& driver_operations, MonitoringChannel channel,
             MonitoringImplementationId id, MonitoringSamplePhase phase,
             ReadProxy<Payload>& proxy)
    -> std::expected<TimestampedMonitoringAcquisition<Payload>, core::Error> {
    auto clock = MonitoringClockOperations::create(driver_operations.context,
                                                   driver_operations.now);
    if (!clock) return std::unexpected{clock.error()};
    auto source = MonitoringSourceOperations<Payload>::create(&proxy, proxy_support,
                                                              proxy_read<Payload>);
    if (!source) return std::unexpected{source.error()};
    return acquire_monitoring_sample(channel, std::move(id), phase, *clock, *source);
}

} // namespace

MonitoringExecutionPolicy::MonitoringExecutionPolicy(
    std::array<MonitoringChannelDisposition, 4> dispositions, bool before, bool after, bool during,
    std::optional<core::Duration> interval, OptionalUnavailableSourcePolicy optional) noexcept
    : dispositions_{dispositions}, before_{before}, after_{after}, during_{during},
      interval_{interval}, optional_unsupported_{optional} {}

auto MonitoringExecutionPolicy::create(
    MonitoringChannelDisposition page, MonitoringChannelDisposition context,
    MonitoringChannelDisposition temperature, MonitoringChannelDisposition frequency, bool before,
    bool after, bool during, std::optional<core::Duration> interval,
    OptionalUnavailableSourcePolicy optional)
    -> std::expected<MonitoringExecutionPolicy, core::Error> {
    const auto enabled = [](MonitoringChannelDisposition value) {
        return value != MonitoringChannelDisposition::disabled;
    };
    if (during != interval.has_value() || (interval && interval->value() == 0) ||
        (enabled(page) && (!before || !after)) ||
        ((enabled(temperature) || enabled(frequency)) && !during)) {
        return std::unexpected{
            driver_error("monitoring.driver.invalid_policy", "channel_sampling_policy")};
    }
    return MonitoringExecutionPolicy{{page, context, temperature, frequency}, before, after,
                                     during, interval, optional};
}

auto MonitoringExecutionPolicy::disposition(MonitoringChannel channel) const noexcept
    -> MonitoringChannelDisposition {
    return index(channel) < 4 ? dispositions_[index(channel)]
                              : MonitoringChannelDisposition::disabled;
}
auto MonitoringExecutionPolicy::before_observation() const noexcept -> bool { return before_; }
auto MonitoringExecutionPolicy::after_observation() const noexcept -> bool { return after_; }
auto MonitoringExecutionPolicy::during_observation() const noexcept -> bool { return during_; }
auto MonitoringExecutionPolicy::during_interval() const noexcept
    -> const std::optional<core::Duration>& { return interval_; }
auto MonitoringExecutionPolicy::optional_unsupported() const noexcept
    -> OptionalUnavailableSourcePolicy { return optional_unsupported_; }

MonitoringWorkerScope::MonitoringWorkerScope(core::WorkerId worker,
                                             platform_linux::LinuxThreadId thread,
                                             core::LinuxCpuId cpu) noexcept
    : worker_id_{worker}, thread_id_{thread}, cpu_{cpu} {}
auto MonitoringWorkerScope::create(core::WorkerId worker, platform_linux::LinuxThreadId thread,
                                   core::LinuxCpuId cpu) noexcept -> MonitoringWorkerScope {
    return {worker, thread, cpu};
}
auto MonitoringWorkerScope::worker_id() const noexcept -> core::WorkerId { return worker_id_; }
auto MonitoringWorkerScope::linux_cpu() const noexcept -> core::LinuxCpuId { return cpu_; }
auto MonitoringWorkerScope::operational_thread_id() const noexcept
    -> platform_linux::LinuxThreadId { return thread_id_; }
auto MonitoringWorkerScope::operator==(const MonitoringWorkerScope& other) const noexcept -> bool {
    return worker_id_ == other.worker_id_ && thread_id_ == other.thread_id_ && cpu_ == other.cpu_;
}

MonitoringSourcePreparationRequest::MonitoringSourcePreparationRequest(
    const MonitoringExecutionPolicy& policy,
    std::span<const MonitoringWorkerScope> workers) noexcept
    : policy_{&policy}, workers_{workers} {}
auto MonitoringSourcePreparationRequest::create(
    const MonitoringExecutionPolicy& policy,
    std::span<const MonitoringWorkerScope> workers) noexcept
    -> MonitoringSourcePreparationRequest {
    return {policy, workers};
}
auto MonitoringSourcePreparationRequest::policy() const noexcept
    -> const MonitoringExecutionPolicy& { return *policy_; }
auto MonitoringSourcePreparationRequest::workers() const noexcept
    -> std::span<const MonitoringWorkerScope> { return workers_; }

struct MonitoringAcquisitionDriver::State {
    MonitoringExecutionPolicy policy;
    std::vector<MonitoringWorkerScope> workers;
    std::vector<MonitoringFrequencyDomain> domains;
    std::array<MonitoringChannelRunDisposition, 4> dispositions;
    std::optional<core::LinuxCpuId> service_cpu;
    MonitoringDriverOperations operations;
    std::jthread thread;
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool ready{};
    bool terminal{};
    bool active{};
    bool arm_requested{};
    bool armed{};
    bool finish_requested{};
    bool abort_requested{};
    bool stop_requested{};
    bool joined{};
    std::uint64_t generation{};
    std::optional<core::Error> service_error;
    std::shared_ptr<detail::MonitoringCaptureRegistration> registration;
    core::Error active_capture_stop_error;
    core::Error stop_contained_error;
    core::Error stop_unknown_error;
    platform_linux::RawMonotonicTimestamp origin =
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(0);
    MonitoringRawCaptureEvidence raw;
    MonitoringDriverActivityEvidence activity;
    std::vector<platform_linux::RawMonotonicTimestamp> context_coverage_start;
    std::vector<platform_linux::RawMonotonicTimestamp> context_coverage_end;
    std::vector<ContextSwitchCaptureCompleteness> context_completeness;

    State(MonitoringExecutionPolicy value, std::vector<MonitoringWorkerScope> worker_values,
          std::optional<core::LinuxCpuId> cpu, MonitoringDriverOperations ops)
        : policy{std::move(value)}, workers{std::move(worker_values)}, service_cpu{cpu},
          operations{ops}, registration{std::make_shared<detail::MonitoringCaptureRegistration>()},
          active_capture_stop_error{driver_error("monitoring.driver.invalid_state", "active_capture")},
          stop_contained_error{driver_error("monitoring.driver.contained_exception", "stop_and_join")},
          stop_unknown_error{driver_error("monitoring.driver.contained_unknown_failure", "stop_and_join")} {}

    ~State() noexcept {
        // Last exclusive ownership, after service retirement/join: contexts
        // are no longer borrowed or reportable. The three fixed contexts have
        // depth one; arbitrary callback context depth requires iterative
        // retirement before JSON's allocating flattening destructor.
        core::detail::retire_json(active_capture_stop_error.context);
        core::detail::retire_json(stop_contained_error.context);
        core::detail::retire_json(stop_unknown_error.context);
        if (service_error) retire_source_context(service_error->context);
    }
};

namespace {

void retire_after_contained_failure(MonitoringAcquisitionDriver::State* state) noexcept {
    if (state == nullptr) return;
    try {
        if (state->thread.joinable()) {
            std::unique_lock lock{state->mutex};
            if (state->arm_requested || state->armed) {
                state->abort_requested = true;
                state->operations.wake(state->operations.context);
                state->condition.notify_all();
                state->condition.wait(lock, [&] {
                    return (!state->arm_requested && !state->armed) || state->service_error;
                });
            }
            state->arm_requested = false;
            state->finish_requested = false;
            state->abort_requested = false;
        }
    } catch (...) {}
    state->active = false;
}

void unregister_capture(MonitoringAcquisitionDriver::State* state,
                        std::uint64_t generation) noexcept {
    if (state == nullptr || !state->registration ||
        state->registration->state != detail::MonitoringCaptureRegistrationState::active ||
        state->registration->generation != generation) std::terminate();
    state->registration->state = detail::MonitoringCaptureRegistrationState::unregistered;
    state->registration->generation = 0;
    state->active = false;
}

void stop_state_after_contained_create(MonitoringAcquisitionDriver::State* state) noexcept {
    if (state == nullptr || !state->thread.joinable()) return;
    try {
        { std::lock_guard lock{state->mutex}; state->stop_requested = true; }
        if (state->operations.wake) state->operations.wake(state->operations.context);
        state->condition.notify_all();
        state->thread.join();
    } catch (...) {}
}

auto active(const MonitoringAcquisitionDriver::State& state, MonitoringChannel channel) {
    return state.dispositions[index(channel)].effective ==
           MonitoringEffectiveChannelDisposition::active;
}

auto now(MonitoringAcquisitionDriver::State& state)
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    if (state.operations.now == nullptr)
        return std::unexpected{driver_error("monitoring.driver.invalid_policy", "missing_now")};
    return state.operations.now(state.operations.context);
}

auto drain_context_round(MonitoringAcquisitionDriver::State& state, bool retain)
    -> std::expected<void, core::Error> {
    if (!active(state, MonitoringChannel::context_switches)) return {};
    for (std::size_t i = 0; i < state.workers.size(); ++i) {
        const auto worker = state.workers[i];
        struct Context { MonitoringAcquisitionDriver::State* state; MonitoringWorkerScope worker; };
        Context context{&state, worker};
        ReadProxy<ContextSwitchEventBatch> proxy{
            &context,
            [](void* opaque) noexcept -> std::expected<ContextSwitchEventBatch, core::Error> {
                auto& value = *static_cast<Context*>(opaque);
                return value.state->operations.drain_context_switch(
                    value.state->operations.context, value.worker.worker_id(),
                    value.worker.operational_thread_id());
            }};
        auto value = acquire(state.operations, MonitoringChannel::context_switches,
                             ContextSwitchEventSource::implementation_id(),
                             MonitoringSamplePhase::during_observation, proxy);
        ++state.activity.drains_;
        if (!value) return std::unexpected{value.error()};
        if (value->payload()->completeness() == ContextSwitchCaptureCompleteness::incomplete)
            state.context_completeness[i] = ContextSwitchCaptureCompleteness::incomplete;
        state.context_coverage_end[i] = value->collection_start();
        if (retain) {
            const auto position = state.raw.context_worker_offsets_[i + 1U];
            std::vector<MonitoringRawCaptureEvidence::ContextAcquisition> reordered;
            reordered.reserve(state.raw.context_.size() + 1U);
            for (std::size_t index{}; index < position; ++index)
                reordered.push_back(std::move(state.raw.context_[index]));
            reordered.push_back(std::move(*value));
            for (std::size_t index = position; index < state.raw.context_.size(); ++index)
                reordered.push_back(std::move(state.raw.context_[index]));
            state.raw.context_ = std::move(reordered);
            for (std::size_t offset = i + 1U;
                 offset < state.raw.context_worker_offsets_.size(); ++offset)
                ++state.raw.context_worker_offsets_[offset];
        }
    }
    return {};
}

auto point_round(MonitoringAcquisitionDriver::State& state)
    -> std::expected<void, core::Error> {
    if (auto drained = drain_context_round(state, true); !drained)
        return drained;
    if (active(state, MonitoringChannel::temperature)) {
        ReadProxy<TemperatureSnapshot> proxy{
            &state,
            [](void* opaque) noexcept -> std::expected<TemperatureSnapshot, core::Error> {
                auto& value = *static_cast<MonitoringAcquisitionDriver::State*>(opaque);
                return value.operations.read_temperature(value.operations.context);
            }};
        auto value = acquire(state.operations, MonitoringChannel::temperature,
                             TemperatureSource::implementation_id(),
                             MonitoringSamplePhase::during_observation, proxy);
        ++state.activity.temperature_;
        if (!value) return std::unexpected{value.error()};
        state.raw.temperature_.push_back(std::move(*value));
    }
    for (std::size_t i = 0; i < state.domains.size(); ++i) {
        if (!active(state, MonitoringChannel::frequency)) break;
        struct Context { MonitoringAcquisitionDriver::State* state; std::size_t index; };
        Context context{&state, i};
        ReadProxy<FrequencySnapshot> proxy{
            &context,
            [](void* opaque) noexcept -> std::expected<FrequencySnapshot, core::Error> {
                auto& value = *static_cast<Context*>(opaque);
                return value.state->operations.read_frequency(
                    value.state->operations.context,
                    value.state->domains[value.index].metadata.domain_id());
            }};
        auto value = acquire(state.operations, MonitoringChannel::frequency,
                             state.domains[i].implementation_id,
                             MonitoringSamplePhase::during_observation, proxy);
        ++state.activity.frequency_;
        if (!value) return std::unexpected{value.error()};
        state.raw.frequency_storage_[i].push_back(std::move(*value));
    }
    return {};
}

void service_main(MonitoringAcquisitionDriver::State* state, std::stop_token) noexcept {
    try {
        inject_if(detail::AcquisitionDriverExceptionBoundary::service);
        if (state->operations.bind_service_cpu == nullptr) {
            std::lock_guard lock{state->mutex};
            state->service_error = driver_error("monitoring.driver.service_start_failed",
                                                "missing_service_binding");
            state->condition.notify_all();
            return;
        }
        auto bound = state->operations.bind_service_cpu(state->operations.context,
                                                        *state->service_cpu);
        if (!bound) {
            std::lock_guard lock{state->mutex};
            state->service_error = bound.error();
            state->condition.notify_all();
            return;
        }
        {
            std::lock_guard lock{state->mutex};
            state->ready = true;
            state->condition.notify_all();
        }
        for (;;) {
            std::unique_lock lock{state->mutex};
            state->condition.wait(lock, [&] { return state->arm_requested || state->stop_requested; });
            if (state->stop_requested) break;
            lock.unlock();

            auto clean = drain_context_round(*state, false);
            auto origin = clean ? now(*state) : std::expected<platform_linux::RawMonotonicTimestamp, core::Error>{std::unexpected{clean.error()}};
            if (origin) {
                state->origin = *origin;
                // The arm drain discards history from outside this capture.  A
                // loss marker in that discarded epoch cannot contaminate the
                // new measured coverage epoch.
                std::ranges::fill(state->context_completeness,
                                  ContextSwitchCaptureCompleteness::complete);
                for (auto& coverage : state->context_coverage_start) coverage = *origin;
                ++state->activity.scheduled_;
                clean = point_round(*state);
                if (clean) ++state->activity.completed_;
            }
            lock.lock();
            if (!clean) state->service_error = clean.error();
            state->arm_requested = false;
            state->armed = clean.has_value();
            state->condition.notify_all();
            lock.unlock();
            if (!clean) continue;

            auto acquisition_interval = state->policy.during_interval();
            if (acquisition_interval &&
                (state->policy.disposition(MonitoringChannel::temperature) !=
                     MonitoringChannelDisposition::disabled ||
                 state->policy.disposition(MonitoringChannel::frequency) !=
                     MonitoringChannelDisposition::disabled)) {
                acquisition_interval = core::Duration::from_base_units(
                    detail::point_acquisition_period_nanoseconds(
                        acquisition_interval->value()));
            }
            std::uint64_t round = 1;
            bool retain_final_drain{};
            bool perform_final_drain{};
            while (true) {
                lock.lock();
                if (state->finish_requested || state->abort_requested || state->stop_requested) {
                    retain_final_drain = state->finish_requested && !state->abort_requested &&
                                         !state->stop_requested;
                    perform_final_drain = !state->stop_requested;
                    lock.unlock();
                    break;
                }
                lock.unlock();
                if (!acquisition_interval) {
                    lock.lock();
                    state->condition.wait(lock, [&] { return state->finish_requested ||
                                                              state->abort_requested ||
                                                              state->stop_requested; });
                    retain_final_drain = state->finish_requested && !state->abort_requested &&
                                         !state->stop_requested;
                    perform_final_drain = !state->stop_requested;
                    lock.unlock();
                    break;
                }
                auto deadline = checked_deadline(state->origin, *acquisition_interval, round);
                if (!deadline) { state->service_error = deadline.error(); break; }
                auto waited = state->operations.wait_until(state->operations.context, *deadline);
                ++state->activity.wakeups_;
                if (!waited) { state->service_error = waited.error(); break; }
                if (*waited == MonitoringSchedulerWaitResult::interrupted) continue;
                auto current = now(*state);
                if (!current) { state->service_error = current.error(); break; }
                auto next = checked_deadline(state->origin, *acquisition_interval, round + 1);
                if (!next) { state->service_error = next.error(); break; }
                if (*current >= *next) {
                    while (*current >= *next) {
                        ++state->activity.missed_;
                        ++round;
                        next = checked_deadline(state->origin, *acquisition_interval, round + 1);
                        if (!next) break;
                    }
                    continue;
                }
                ++state->activity.scheduled_;
                auto value = point_round(*state);
                if (!value) { state->service_error = value.error(); break; }
                ++state->activity.completed_;
                ++round;
            }
            std::expected<void, core::Error> final{};
            if (perform_final_drain) final = drain_context_round(*state, retain_final_drain);
            lock.lock();
            if (!final && !state->service_error) state->service_error = final.error();
            state->armed = false;
            state->finish_requested = false;
            state->abort_requested = false;
            state->condition.notify_all();
            lock.unlock();
        }
    } catch (const std::exception&) {
        std::lock_guard lock{state->mutex};
        state->service_error = driver_error("monitoring.driver.contained_exception",
                                            "service_exception");
        state->condition.notify_all();
    } catch (...) {
        std::lock_guard lock{state->mutex};
        state->service_error = driver_error("monitoring.driver.contained_unknown_failure",
                                            "service_exception");
        state->condition.notify_all();
    }
}

} // namespace

MonitoringAcquisitionDriver::MonitoringAcquisitionDriver(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

auto MonitoringAcquisitionDriver::create(MonitoringExecutionPolicy policy,
                                         std::vector<MonitoringWorkerScope> workers,
    std::optional<core::LinuxCpuId> service_cpu,
                                         MonitoringDriverOperations operations)
    -> std::expected<MonitoringAcquisitionDriver, core::Error> {
    std::unique_ptr<State> state;
    try {
    inject_if(detail::AcquisitionDriverExceptionBoundary::create);
    if (workers.empty() || operations.now == nullptr || operations.prepare_sources == nullptr ||
        operations.support == nullptr)
        return std::unexpected{driver_error("monitoring.driver.invalid_policy", "construction")};
    if (!std::ranges::is_sorted(workers, {}, &MonitoringWorkerScope::worker_id) ||
        std::ranges::adjacent_find(workers, {}, &MonitoringWorkerScope::worker_id) != workers.end())
        return std::unexpected{driver_error("monitoring.driver.invalid_policy", "worker_order")};
    state = std::make_unique<State>(std::move(policy), std::move(workers), service_cpu, operations);
    const auto preparation_request = MonitoringSourcePreparationRequest::create(
        state->policy, state->workers);
    auto prepared = operations.prepare_sources(operations.context, preparation_request);
    if (!prepared) return std::unexpected{std::move(prepared.error())};
    const std::array channels{MonitoringChannel::page_faults, MonitoringChannel::context_switches,
                              MonitoringChannel::temperature, MonitoringChannel::frequency};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const auto authored = state->policy.disposition(channels[i]);
        auto effective = MonitoringEffectiveChannelDisposition::disabled;
        if (authored != MonitoringChannelDisposition::disabled) {
            const auto support = operations.support(operations.context, channels[i]);
            if (support == MonitoringSourceSupport::supported) effective = MonitoringEffectiveChannelDisposition::active;
            else if (authored == MonitoringChannelDisposition::required ||
                     state->policy.optional_unsupported() == OptionalUnavailableSourcePolicy::fail)
                return std::unexpected{driver_error("monitoring.driver.required_source_unsupported",
                                                    "source_unsupported")};
            else if (state->policy.optional_unsupported() == OptionalUnavailableSourcePolicy::warn)
                effective = MonitoringEffectiveChannelDisposition::unsupported_warning;
        }
        state->dispositions[i] = {channels[i], authored, effective};
    }
    if (active(*state, MonitoringChannel::frequency)) {
        auto& domains = prepared->frequency_domains;
        std::vector<std::size_t> order(domains.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::sort(order, {}, [&](std::size_t i) -> const FrequencyDomainId& {
            return domains[i].metadata.domain_id();
        });
        std::vector<MonitoringFrequencyDomain> canonical;
        canonical.reserve(order.size());
        for (const auto i : order) {
            if (!canonical.empty() && canonical.back().metadata.domain_id() ==
                                          domains[i].metadata.domain_id())
                continue;
            canonical.push_back(std::move(domains[i]));
        }
        state->domains = std::move(canonical);
        for (const auto& domain : state->domains) state->raw.frequency_metadata_.push_back(domain.metadata);
        state->raw.frequency_storage_.resize(state->domains.size());
    }
    state->context_coverage_start.resize(state->workers.size(),
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(0));
    state->context_coverage_end.resize(state->workers.size(),
        platform_linux::RawMonotonicTimestamp::from_nanoseconds(0));
    state->context_completeness.resize(state->workers.size(), ContextSwitchCaptureCompleteness::complete);
    state->raw.context_worker_offsets_.resize(state->workers.size() + 1U);
    const bool asynchronous = active(*state, MonitoringChannel::context_switches) ||
                              active(*state, MonitoringChannel::temperature) ||
                              active(*state, MonitoringChannel::frequency);
    if (asynchronous != service_cpu.has_value() ||
        (asynchronous && (operations.bind_service_cpu == nullptr || operations.wait_until == nullptr ||
                          operations.wake == nullptr)))
        return std::unexpected{driver_error("monitoring.driver.invalid_policy", "service_cpu")};
    if (asynchronous) {
        auto* pointer = state.get();
        state->thread = std::jthread{[pointer](std::stop_token token) { service_main(pointer, token); }};
        std::unique_lock lock{state->mutex};
        state->condition.wait(lock, [&] { return state->ready || state->service_error.has_value(); });
        if (state->service_error) {
            state->stop_requested = true;
            lock.unlock();
            operations.wake(operations.context);
            state->condition.notify_all();
            state->thread.join();
            return std::unexpected{*state->service_error};
        }
    } else state->ready = true;
    return MonitoringAcquisitionDriver{std::move(state)};
    } catch (const std::exception&) {
        stop_state_after_contained_create(state.get());
        return std::unexpected{driver_error("monitoring.driver.contained_exception", "create")};
    } catch (...) {
        stop_state_after_contained_create(state.get());
        return std::unexpected{driver_error("monitoring.driver.contained_unknown_failure", "create")};
    }
}

auto MonitoringAcquisitionDriver::begin_capture()
    -> std::expected<MonitoringObservationCapture, core::Error> {
    try {
    inject_if(detail::AcquisitionDriverExceptionBoundary::begin);
    if (!state_ || state_->terminal || state_->active || state_->service_error ||
        state_->registration->state != detail::MonitoringCaptureRegistrationState::unregistered)
        return std::unexpected{driver_error("monitoring.driver.invalid_state", "begin_capture")};
    if (state_->generation == std::numeric_limits<std::uint64_t>::max()) {
        state_->terminal = true;
        return std::unexpected{driver_error("monitoring.driver.invariant_failed", "generation_exhausted")};
    }
    state_->raw.page_before_.clear(); state_->raw.page_after_.clear(); state_->raw.context_.clear();
    std::ranges::fill(state_->raw.context_worker_offsets_, 0U);
    state_->raw.temperature_.clear();
    for (auto& values : state_->raw.frequency_storage_) values.clear();
    state_->activity = {};
    if (active(*state_, MonitoringChannel::page_faults)) {
        for (const auto& worker : state_->workers) {
            struct Context { State* state; MonitoringWorkerScope worker; } context{state_.get(), worker};
            ReadProxy<PageFaultSnapshot> proxy{&context, [](void* opaque) noexcept -> std::expected<PageFaultSnapshot, core::Error> {
                auto& value = *static_cast<Context*>(opaque);
                return value.state->operations.read_page_fault(value.state->operations.context,
                                                               value.worker.worker_id(),
                                                               value.worker.operational_thread_id());
            }};
            auto value = acquire(state_->operations, MonitoringChannel::page_faults,
                                 PageFaultSource::implementation_id(),
                                 MonitoringSamplePhase::before_observation, proxy);
            if (!value) return std::unexpected{value.error()};
            state_->raw.page_before_.push_back(std::move(*value));
        }
    }
    state_->registration->generation = state_->generation + 1;
    state_->registration->state = detail::MonitoringCaptureRegistrationState::active;
    state_->active = true;
    inject_if(detail::AcquisitionDriverExceptionBoundary::begin_after_active);
    ++state_->generation;
    if (state_->thread.joinable()) {
        std::unique_lock lock{state_->mutex};
        state_->arm_requested = true;
        state_->condition.notify_all();
        state_->condition.wait(lock, [&] { return state_->armed || state_->service_error; });
        if (state_->service_error) {
            const auto error = *state_->service_error;
            lock.unlock();
            unregister_capture(state_.get(), state_->generation);
            return std::unexpected{error};
        }
    }
    return MonitoringObservationCapture{state_.get(), state_->generation, state_->registration};
    } catch (const std::exception&) {
        retire_after_contained_failure(state_.get());
        if (state_ && state_->registration->state ==
                          detail::MonitoringCaptureRegistrationState::active) {
            state_->registration->state = detail::MonitoringCaptureRegistrationState::unregistered;
            state_->registration->generation = 0;
        }
        return std::unexpected{driver_error("monitoring.driver.contained_exception", "begin_capture")};
    } catch (...) {
        retire_after_contained_failure(state_.get());
        if (state_ && state_->registration->state ==
                          detail::MonitoringCaptureRegistrationState::active) {
            state_->registration->state = detail::MonitoringCaptureRegistrationState::unregistered;
            state_->registration->generation = 0;
        }
        return std::unexpected{driver_error("monitoring.driver.contained_unknown_failure", "begin_capture")};
    }
}

auto MonitoringAcquisitionDriver::stop_and_join() -> std::expected<void, core::Error> {
    std::optional<core::Error> active_capture_error;
    const auto force_terminal = [this]() noexcept {
        if (!state_) return;
        try {
            state_->active = false;
            if (state_->thread.joinable()) {
                {
                    std::lock_guard lock{state_->mutex}; state_->stop_requested = true;
                }
                try { state_->operations.wake(state_->operations.context); } catch (...) {}
                state_->condition.notify_all();
                state_->thread.request_stop();
                state_->thread.join();
                state_->thread = std::jthread{};
            }
            state_->joined = true;
            state_->terminal = true;
            state_->arm_requested = false;
            state_->armed = false;
            state_->finish_requested = false;
            state_->abort_requested = false;
        } catch (...) {
            if (state_->thread.joinable()) std::terminate();
            state_->joined = true;
            state_->terminal = true;
            state_->arm_requested = false;
            state_->armed = false;
            state_->finish_requested = false;
            state_->abort_requested = false;
        }
    };
    try {
    if (state_ && state_->active) {
        active_capture_error.emplace(std::move(state_->active_capture_stop_error));
        if (state_->registration->state != detail::MonitoringCaptureRegistrationState::active ||
            state_->registration->generation != state_->generation) std::terminate();
        {
            std::lock_guard lock{state_->mutex};
            state_->stop_requested = true;
        }
        state_->registration->state = detail::MonitoringCaptureRegistrationState::retired;
        inject_if(detail::AcquisitionDriverExceptionBoundary::stop_retirement);
        retire_after_contained_failure(state_.get());
    }
    inject_if(detail::AcquisitionDriverExceptionBoundary::stop);
    if (!state_ || state_->joined) return {};
    if (state_->thread.joinable()) {
        {
            std::lock_guard lock{state_->mutex}; state_->stop_requested = true;
        }
        inject_if(detail::AcquisitionDriverExceptionBoundary::stop_wake);
        state_->operations.wake(state_->operations.context);
        state_->condition.notify_all();
        inject_if(detail::AcquisitionDriverExceptionBoundary::stop_join);
        state_->thread.join();
        state_->thread = std::jthread{};
    }
    inject_if(detail::AcquisitionDriverExceptionBoundary::stop_terminalization);
    state_->joined = true; state_->terminal = true;
    state_->arm_requested = false; state_->armed = false;
    state_->finish_requested = false; state_->abort_requested = false;
    if (active_capture_error) return std::unexpected{std::move(*active_capture_error)};
    if (state_->service_error) return std::unexpected{std::move(*state_->service_error)};
    return {};
    } catch (const std::exception&) {
        if (state_ && state_->registration &&
            state_->registration->state == detail::MonitoringCaptureRegistrationState::active) {
            state_->registration->state = detail::MonitoringCaptureRegistrationState::retired;
        }
        force_terminal();
        if (active_capture_error) return std::unexpected{std::move(*active_capture_error)};
        return std::unexpected{state_ ? std::move(state_->stop_contained_error)
                                      : driver_error("monitoring.driver.contained_exception",
                                                     "stop_and_join")};
    } catch (...) {
        if (state_ && state_->registration &&
            state_->registration->state == detail::MonitoringCaptureRegistrationState::active) {
            state_->registration->state = detail::MonitoringCaptureRegistrationState::retired;
        }
        force_terminal();
        if (active_capture_error) return std::unexpected{std::move(*active_capture_error)};
        return std::unexpected{state_ ? std::move(state_->stop_unknown_error)
                                      : driver_error("monitoring.driver.contained_unknown_failure",
                                                     "stop_and_join")};
    }
}
auto MonitoringAcquisitionDriver::dispositions() const noexcept
    -> std::span<const MonitoringChannelRunDisposition> { return state_->dispositions; }
auto MonitoringAcquisitionDriver::ready() const noexcept -> bool {
    return state_ && state_->ready && !state_->terminal && !state_->active && !state_->service_error;
}
auto MonitoringAcquisitionDriver::has_service_thread() const noexcept -> bool {
    return state_ && state_->thread.joinable();
}
MonitoringAcquisitionDriver::MonitoringAcquisitionDriver(MonitoringAcquisitionDriver&& other) noexcept
    : state_{std::move(other.state_)} {}
MonitoringAcquisitionDriver::~MonitoringAcquisitionDriver() noexcept {
    if (state_ && !state_->joined) std::terminate();
}

MonitoringObservationCapture::MonitoringObservationCapture(
    MonitoringAcquisitionDriver::State* state, std::uint64_t generation,
    std::shared_ptr<detail::MonitoringCaptureRegistration> registration) noexcept
    : state_{state}, registration_{std::move(registration)}, generation_{generation} {}
MonitoringObservationCapture::MonitoringObservationCapture(MonitoringObservationCapture&& other) noexcept
    : state_{std::exchange(other.state_, nullptr)}, registration_{std::move(other.registration_)},
      generation_{other.generation_},
      terminal_{std::exchange(other.terminal_, true)} {}
MonitoringObservationCapture::~MonitoringObservationCapture() noexcept {
    if (!registration_) return;
    if (terminal_) return;
    if (registration_->state == detail::MonitoringCaptureRegistrationState::retired &&
        registration_->generation == generation_) {
        registration_->state = detail::MonitoringCaptureRegistrationState::unregistered;
        registration_->generation = 0;
        return;
    }
    std::terminate();
}
auto MonitoringObservationCapture::generation() const noexcept -> std::uint64_t { return generation_; }

auto MonitoringObservationCapture::abort() -> std::expected<void, core::Error> {
    try {
    inject_if(detail::AcquisitionDriverExceptionBoundary::abort);
    if (!state_ || terminal_ || !state_->active || state_->generation != generation_)
        return std::unexpected{driver_error("monitoring.driver.invalid_state", "abort")};
    if (state_->thread.joinable()) {
        std::unique_lock lock{state_->mutex}; state_->abort_requested = true;
        state_->operations.wake(state_->operations.context); state_->condition.notify_all();
        state_->condition.wait(lock, [&] { return !state_->armed || state_->service_error; });
    }
    auto error = state_->service_error;
    unregister_capture(state_, generation_); terminal_ = true; state_ = nullptr; registration_.reset();
    if (error) return std::unexpected{*error};
    return {};
    } catch (const std::exception&) {
        retire_after_contained_failure(state_);
        unregister_capture(state_, generation_); terminal_ = true; state_ = nullptr; registration_.reset();
        return std::unexpected{driver_error("monitoring.driver.contained_exception", "abort")};
    } catch (...) {
        retire_after_contained_failure(state_);
        unregister_capture(state_, generation_); terminal_ = true; state_ = nullptr; registration_.reset();
        return std::unexpected{driver_error("monitoring.driver.contained_unknown_failure", "abort")};
    }
}

auto MonitoringObservationCapture::complete(platform_linux::RawMonotonicTimestamp start,
                                             platform_linux::RawMonotonicTimestamp end)
    -> std::expected<MonitoringCaptureResult, core::Error> {
    try {
    inject_if(detail::AcquisitionDriverExceptionBoundary::complete);
    if (!state_ || terminal_ || !state_->active || state_->generation != generation_ || end < start)
        return std::unexpected{driver_error("monitoring.driver.invalid_state", "complete")};
    const auto retire_failed_completion = [this](core::Error error)
        -> std::expected<MonitoringCaptureResult, core::Error> {
        if (state_->thread.joinable()) {
            std::unique_lock lock{state_->mutex};
            state_->abort_requested = true;
            state_->operations.wake(state_->operations.context);
            state_->condition.notify_all();
            state_->condition.wait(lock, [&] { return !state_->armed || state_->service_error; });
        }
        unregister_capture(state_, generation_);
        terminal_ = true; state_ = nullptr; registration_.reset();
        return std::unexpected{std::move(error)};
    };
    if (active(*state_, MonitoringChannel::page_faults)) {
        for (const auto& worker : state_->workers) {
            struct Context { MonitoringAcquisitionDriver::State* state; MonitoringWorkerScope worker; }
                context{state_, worker};
            ReadProxy<PageFaultSnapshot> proxy{&context, [](void* opaque) noexcept -> std::expected<PageFaultSnapshot, core::Error> {
                auto& value = *static_cast<Context*>(opaque);
                return value.state->operations.read_page_fault(value.state->operations.context,
                                                               value.worker.worker_id(),
                                                               value.worker.operational_thread_id());
            }};
            auto value = acquire(state_->operations, MonitoringChannel::page_faults,
                                 PageFaultSource::implementation_id(),
                                 MonitoringSamplePhase::after_observation, proxy);
            if (!value) return retire_failed_completion(value.error());
            state_->raw.page_after_.push_back(std::move(*value));
        }
    }
    if (state_->thread.joinable()) {
        std::unique_lock lock{state_->mutex}; state_->finish_requested = true;
        state_->operations.wake(state_->operations.context); state_->condition.notify_all();
        state_->condition.wait(lock, [&] { return !state_->armed || state_->service_error; });
    }
    if (state_->service_error) return retire_failed_completion(*state_->service_error);

    MonitoringCaptureResult result;
    for (std::size_t i = 0; i < 4; ++i) {
        result.evidence_.states_[i] = state_->dispositions[i].effective == MonitoringEffectiveChannelDisposition::unsupported_warning
                                          ? MonitoringSignalState::unavailable
                                          : MonitoringSignalState::not_applicable;
    }
    if (active(*state_, MonitoringChannel::page_faults)) {
        std::vector<PageFaultAcquisitionPair> pairs;
        for (std::size_t i = 0; i < state_->workers.size(); ++i)
            pairs.emplace_back(state_->workers[i].worker_id(), state_->raw.page_before_[i], state_->raw.page_after_[i]);
        auto evidence = classify_page_faults(start, end, pairs);
        if (!evidence) return retire_failed_completion(evidence.error());
        result.evidence_.states_[0] = evidence->state(); result.evidence_.page_ = std::move(*evidence);
    }
    if (active(*state_, MonitoringChannel::context_switches)) {
        std::vector<core::WorkerId> workers; std::vector<ContextSwitchWorkerAcquisitionView> views;
        for (std::size_t i = 0; i < state_->workers.size(); ++i) {
            workers.push_back(state_->workers[i].worker_id());
            const auto first = state_->raw.context_worker_offsets_[i];
            const auto last = state_->raw.context_worker_offsets_[i + 1U];
            views.emplace_back(state_->workers[i].worker_id(), ContextSwitchEventSource::implementation_id(),
                               state_->context_coverage_start[i], state_->context_coverage_end[i],
                               state_->context_completeness[i],
                               std::span{state_->raw.context_}.subspan(first, last - first));
        }
        auto evidence = classify_context_switches(start, end, workers, views);
        if (!evidence) return retire_failed_completion(evidence.error());
        result.evidence_.states_[1] = evidence->state(); result.evidence_.context_ = std::move(*evidence);
    }
    const auto completeness = state_->activity.missed_ == 0;
    if (active(*state_, MonitoringChannel::temperature)) {
        auto selected = inside_points(std::span<const TimestampedMonitoringAcquisition<TemperatureSnapshot>>{
            state_->raw.temperature_}, start, end,
            MonitoringChannel::temperature, [](const auto& item) -> std::string_view {
                if (item.channel() != MonitoringChannel::temperature ||
                    item.phase() != MonitoringSamplePhase::during_observation ||
                    item.implementation_id().value() != "linux-hwmon-k10temp-tctl")
                    return "metadata";
                if (item.status() == MonitoringAcquisitionStatus::available &&
                    item.payload()->sensor_kind() != TemperatureSensorKind::cpu_control)
                    return "sensor";
                return {};
            });
        if (!selected) return retire_failed_completion(std::move(selected.error()));
        auto evidence = classify_temperature(start, end, *state_->policy.during_interval(),
            completeness ? TemperatureCaptureCompleteness::complete : TemperatureCaptureCompleteness::incomplete,
            *selected);
        if (!evidence) return retire_failed_completion(evidence.error());
        result.evidence_.states_[2] = evidence->state(); result.evidence_.temperature_ = std::move(*evidence);
    }
    state_->raw.frequency_views_.clear();
    for (std::size_t i = 0; i < state_->domains.size(); ++i)
        state_->raw.frequency_views_.push_back({state_->domains[i].metadata.domain_id(), state_->raw.frequency_storage_[i]});
    if (active(*state_, MonitoringChannel::frequency)) {
        std::vector<FrequencyDomainAcquisitions> classifier_domains;
        classifier_domains.reserve(state_->domains.size());
        for (std::size_t i{}; i < state_->domains.size(); ++i) {
            const auto& domain = state_->domains[i];
            auto selected = inside_points(state_->raw.frequency_views_[i].acquisitions, start, end,
                MonitoringChannel::frequency, [&](const auto& item) -> std::string_view {
                    if (item.channel() != MonitoringChannel::frequency ||
                        item.phase() != MonitoringSamplePhase::during_observation)
                        return "metadata";
                    const auto kind = domain.metadata.source_kind();
                    if (kind != FrequencySourceKind::hardware_current &&
                        kind != FrequencySourceKind::scaling_current)
                        return "source";
                    const auto expected_id = kind == FrequencySourceKind::hardware_current
                        ? "linux-cpufreq-cpuinfo-cur-freq" : "linux-cpufreq-scaling-cur-freq";
                    if (item.implementation_id().value() != expected_id)
                        return "implementation";
                    if (item.status() == MonitoringAcquisitionStatus::available &&
                        (item.payload()->domain_id() != domain.metadata.domain_id() ||
                         item.payload()->source_kind() != kind))
                        return "source";
                    return {};
                });
            if (!selected) return retire_failed_completion(std::move(selected.error()));
            // Empty eligible spans remain real domains; the unchanged
            // classifier decides their completeness/unavailable outcome.
            classifier_domains.push_back({domain.metadata.domain_id(), *selected});
        }
        auto evidence = classify_frequency(start, end, *state_->policy.during_interval(),
            completeness ? FrequencyCaptureCompleteness::complete : FrequencyCaptureCompleteness::incomplete,
            classifier_domains);
        if (!evidence) return retire_failed_completion(evidence.error());
        result.evidence_.states_[3] = evidence->state(); result.evidence_.frequency_ = std::move(*evidence);
    }
    result.raw_ = std::move(state_->raw); result.activity_ = state_->activity;
    state_->raw.context_worker_offsets_.resize(state_->workers.size() + 1U);
    state_->raw.frequency_storage_.resize(state_->domains.size());
    for (const auto& domain : state_->domains)
        state_->raw.frequency_metadata_.push_back(domain.metadata);
    unregister_capture(state_, generation_); terminal_ = true; state_ = nullptr; registration_.reset();
    return result;
    } catch (const std::exception&) {
        retire_after_contained_failure(state_);
        unregister_capture(state_, generation_); terminal_ = true; state_ = nullptr; registration_.reset();
        return std::unexpected{driver_error("monitoring.driver.contained_exception", "complete")};
    } catch (...) {
        retire_after_contained_failure(state_);
        unregister_capture(state_, generation_); terminal_ = true; state_ = nullptr; registration_.reset();
        return std::unexpected{driver_error("monitoring.driver.contained_unknown_failure", "complete")};
    }
}

auto MonitoringDriverActivityEvidence::scheduled_rounds() const noexcept -> std::uint64_t { return scheduled_; }
auto MonitoringDriverActivityEvidence::completed_rounds() const noexcept -> std::uint64_t { return completed_; }
auto MonitoringDriverActivityEvidence::missed_rounds() const noexcept -> std::uint64_t { return missed_; }
auto MonitoringDriverActivityEvidence::service_wakeups() const noexcept -> std::uint64_t { return wakeups_; }
auto MonitoringDriverActivityEvidence::context_drains() const noexcept -> std::uint64_t { return drains_; }
auto MonitoringDriverActivityEvidence::temperature_attempts() const noexcept -> std::uint64_t { return temperature_; }
auto MonitoringDriverActivityEvidence::frequency_attempts() const noexcept -> std::uint64_t { return frequency_; }
MonitoringRawCaptureEvidence::MonitoringRawCaptureEvidence(const MonitoringRawCaptureEvidence& other)
    : page_before_{other.page_before_}, page_after_{other.page_after_},
      context_{other.context_}, context_count_{other.context_count_},
      context_worker_offsets_{other.context_worker_offsets_}, temperature_{other.temperature_},
      frequency_storage_{other.frequency_storage_}, frequency_views_{other.frequency_views_},
      frequency_metadata_{other.frequency_metadata_} {
    // Driver/reconstruction owners pair each domain view with its storage slot.
    // Preserve subranges while rebinding; default vector moves preserve the allocation.
    if (frequency_views_.size() > frequency_storage_.size())
        throw std::logic_error{"invalid frequency capture ownership"};
    for (std::size_t index{}; index < frequency_views_.size(); ++index) {
        const auto& source = other.frequency_storage_[index];
        const auto view = other.frequency_views_[index].acquisitions;
        std::size_t offset{};
        if (!source.empty()) {
            while (offset < source.size() && source.data() + offset != view.data()) ++offset;
            if (source.data() + offset != view.data() || view.size() > source.size() - offset)
                throw std::logic_error{"invalid frequency capture ownership"};
        } else if (!view.empty()) {
            throw std::logic_error{"invalid frequency capture ownership"};
        }
        frequency_views_[index].acquisitions =
            std::span<const FrequencyAcquisition>{frequency_storage_[index]}.subspan(offset, view.size());
    }
}
auto MonitoringRawCaptureEvidence::operator=(const MonitoringRawCaptureEvidence& other)
    -> MonitoringRawCaptureEvidence& {
    if (this != &other) {
        MonitoringRawCaptureEvidence copy{other};
        *this = std::move(copy);
    }
    return *this;
}
auto MonitoringRawCaptureEvidence::page_fault_before() const noexcept -> std::span<const PageAcquisition> { return page_before_; }
auto MonitoringRawCaptureEvidence::page_fault_after() const noexcept -> std::span<const PageAcquisition> { return page_after_; }
auto MonitoringRawCaptureEvidence::context_switches() const noexcept -> std::span<const ContextAcquisition> {
    return context_count_ ? std::span<const ContextAcquisition>{context_}.first(*context_count_)
                          : std::span<const ContextAcquisition>{context_};
}
auto MonitoringRawCaptureEvidence::temperature() const noexcept -> std::span<const TemperatureAcquisition> { return temperature_; }
auto MonitoringRawCaptureEvidence::frequency() const noexcept -> std::span<const FrequencyDomainAcquisitions> { return frequency_views_; }
auto MonitoringRawCaptureEvidence::frequency_metadata() const noexcept -> std::span<const FrequencyPolicyMetadata> { return frequency_metadata_; }
auto MonitoringObservationEvidence::page_faults_state() const noexcept -> MonitoringSignalState { return states_[0]; }
auto MonitoringObservationEvidence::context_switches_state() const noexcept -> MonitoringSignalState { return states_[1]; }
auto MonitoringObservationEvidence::temperature_state() const noexcept -> MonitoringSignalState { return states_[2]; }
auto MonitoringObservationEvidence::frequency_state() const noexcept -> MonitoringSignalState { return states_[3]; }
auto MonitoringObservationEvidence::page_faults() const noexcept -> const std::optional<PageFaultObservationEvidence>& { return page_; }
auto MonitoringObservationEvidence::context_switches() const noexcept -> const std::optional<ContextSwitchObservationEvidence>& { return context_; }
auto MonitoringObservationEvidence::temperature() const noexcept -> const std::optional<TemperatureObservationEvidence>& { return temperature_; }
auto MonitoringObservationEvidence::frequency() const noexcept -> const std::optional<FrequencyObservationEvidence>& { return frequency_; }
auto MonitoringCaptureResult::evidence() const noexcept -> const MonitoringObservationEvidence& { return evidence_; }
auto MonitoringCaptureResult::raw() const noexcept -> const MonitoringRawCaptureEvidence& { return raw_; }
auto MonitoringCaptureResult::activity() const noexcept -> const MonitoringDriverActivityEvidence& { return activity_; }

void detail::AcquisitionDriverAccess::inject(AcquisitionDriverExceptionBoundary boundary,
                                             AcquisitionDriverExceptionKind kind) noexcept {
    injected_kind.store(kind);
    injected_boundary.store(boundary);
}
void detail::AcquisitionDriverAccess::clear() noexcept {
    injected_boundary.store(AcquisitionDriverExceptionBoundary::none);
}

void detail::AcquisitionDriverAccess::corrupt_registration_for_test(
    MonitoringObservationCapture& capture, MonitoringRegistrationTestFault fault) noexcept {
    if (!capture.registration_) std::terminate();
    if (fault == MonitoringRegistrationTestFault::wrong_generation) {
        ++capture.registration_->generation;
    } else {
        capture.registration_->state = MonitoringCaptureRegistrationState::unregistered;
    }
}

void detail::AcquisitionDriverAccess::consume_retired_registration_for_test(
    MonitoringObservationCapture& capture) noexcept {
    if (!capture.registration_ ||
        capture.registration_->state != MonitoringCaptureRegistrationState::retired ||
        capture.registration_->generation != capture.generation_) std::terminate();
    capture.registration_->state = MonitoringCaptureRegistrationState::unregistered;
    capture.registration_->generation = 0;
}

void detail::AcquisitionDriverAccess::inject_service_error_for_test(
    MonitoringAcquisitionDriver& driver, core::Error error) noexcept {
    if (!driver.state_) std::terminate();
    std::lock_guard lock{driver.state_->mutex};
    if (!driver.state_->ready || driver.state_->active || driver.state_->terminal ||
        driver.state_->service_error)
        std::terminate();
    driver.state_->service_error.emplace(std::move(error));
}

auto detail::AcquisitionDriverAccess::snapshot(
    const MonitoringAcquisitionDriver& driver) noexcept -> AcquisitionDriverSnapshot {
    if (!driver.state_) std::terminate();
    std::lock_guard lock{driver.state_->mutex};
    const auto& state = *driver.state_;
    return {.active = state.active,
            .arm_requested = state.arm_requested,
            .armed = state.armed,
            .finish_requested = state.finish_requested,
            .abort_requested = state.abort_requested,
            .joined = state.joined,
            .terminal = state.terminal,
            .service_thread = state.thread.joinable(),
            .registration_state = static_cast<std::uint8_t>(state.registration->state),
            .registration_generation = state.registration->generation,
            .raw_item_count = state.raw.page_before_.size() + state.raw.page_after_.size() +
                              state.raw.context_.size() + state.raw.temperature_.size(),
            .scheduled = state.activity.scheduled_,
            .completed = state.activity.completed_,
            .wakeups = state.activity.wakeups_};
}

} // namespace epycbench::monitoring
