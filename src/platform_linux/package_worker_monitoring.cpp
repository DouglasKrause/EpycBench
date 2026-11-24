#include "platform_linux/package_worker_monitoring.hpp"

#include "epycbench/platform_linux/cpu_frequency.hpp"
#include "epycbench/platform_linux/hwmon_temperature.hpp"
#include "epycbench/platform_linux/perf_context_switch.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"
#include "platform_linux/thread_rusage_operations.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <latch>
#include <map>
#include <mutex>
#include <thread>

namespace epycbench::platform_linux::package_execution {
namespace {
namespace monitor = measurement::package_monitoring;
using monitor::Channel;
using Json = nlohmann::json;
using U64 = std::uint64_t;
using core::LinuxCpuId;

constexpr std::size_t max_poll_samples = 512U;
constexpr std::size_t max_retained_poll_samples = 16384U;

auto raw_now() -> std::expected<U64, core::Error> {
    return raw_monotonic_now().transform([](auto stamp) { return stamp.nanoseconds(); });
}

auto reason(std::string_view kind, std::string_view code) -> Json {
    return {{"kind", kind}, {"code", code}};
}

auto reason(const core::Error& error, bool discovery = false) -> Json {
    const auto native = error.os_error ? error.os_error->value : 0;
    const bool unsupported = discovery &&
        (native == EPERM || native == EACCES || native == ENOSYS || native == EOPNOTSUPP ||
         native == ENODEV || native == ENOENT || native == EINVAL);
    return reason(unsupported ? "unsupported" : "capture_failed", error.code.value());
}

auto empty_channel(Channel channel) -> Json {
    return {{"source", monitor::source_names[static_cast<std::size_t>(channel)]}, {"before", nullptr},
            {"after", nullptr}, {"during", Json::array()}, {"events", Json::array()},
            {"drained_ns", nullptr}, {"unavailable_reason", nullptr},
            {"during_unavailable_reason", nullptr}};
}

void retain_reason(Json& channel, Json detail) {
    auto& existing = channel.at("unavailable_reason");
    if (existing.is_null() || (existing.at("kind") != "capture_failed" && detail.at("kind") == "capture_failed"))
        existing = std::move(detail);
}

template<class T> auto sample(T value) -> Json {
    const auto at = raw_now();
    if (!at) return nullptr;
    return {{"time_ns", *at}, {"value", value}};
}

template<class T> void retain_sample(Json& row, const char* phase, std::expected<T, core::Error> value) {
    if (value) {
        row.at(phase) = sample(*value);
        if (row.at(phase).is_null())
            retain_reason(row, reason("capture_failed", "monitoring_timestamp_unavailable"));
    } else {
        retain_reason(row, reason(value.error()));
    }
}

template<class Source> void prepare_source(Json& row, std::optional<Source>& source,
                                            std::expected<std::optional<Source>, core::Error> discovered) {
    if (!discovered)
        retain_reason(row, reason(discovered.error(), true));
    else if (!*discovered)
        retain_reason(row, reason("unsupported", "source_not_found"));
    else
        source.emplace(std::move(**discovered));
}

auto read_frequency(const CpuFrequencyPolicySource& source) -> std::expected<Json, core::Error> {
    auto value = source.read();
    if (!value) return std::unexpected{value.error()};
    auto cpus = Json::array();
    for (const auto cpu : source.related_cpus()) cpus.push_back(cpu.value());
    return Json{{"hz", *value}, {"policy_cpus", std::move(cpus)},
                {"kind", source.kind() == CpuFrequencyCurrentKind::hardware_current
                             ? "cpuinfo_cur_freq" : "scaling_cur_freq"}};
}

struct PollSeries {
    Json points = Json::array();
    Json unavailable = nullptr;

    void append(const auto& read) {
        if (points.size() >= max_poll_samples) {
            unavailable = reason("capture_failed", "during_observation_sample_capacity");
            return;
        }
        const auto start = raw_now();
        const auto value = read();
        if (!value) unavailable = reason(value.error());
        else if (auto point = sample(*value); start && !point.is_null()) {
            point["read_start_ns"] = *start;
            points.push_back(std::move(point));
        } else
            unavailable = reason("capture_failed", "monitoring_timestamp_unavailable");
    }
};

struct FrequencyTarget {
    std::atomic<unsigned> active{};
    std::optional<CpuFrequencyPolicySource> source;
    PollSeries series;
};
} // namespace

struct PackageMonitor::State {
    monitor::Policy policy;
    bool supplied;
    std::atomic<unsigned> active{};
    std::map<LinuxCpuId, FrequencyTarget> frequency;
    std::optional<HwmonTemperatureSource> thermal_source;
    PollSeries temperature;
    Json service_error = nullptr;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread service;
    std::latch ready{1};
    bool announced{};
    std::size_t retained_points{};

    State(monitor::Policy settings, bool synthetic) : policy{settings}, supplied{synthetic} {}

    void announce() noexcept {
        if (!std::exchange(announced, true)) ready.count_down();
    }

    void run(std::stop_token token) {
        const LinuxCpuId expected{*policy.service_cpu};
        auto pinned = set_current_thread_affinity(core::CpuSet{expected});
        if (!pinned) {
            service_error = reason(pinned.error());
            announce();
            return;
        }
        const auto observed = current_cpu();
        if (!observed || *observed != expected) {
            service_error = reason("capture_failed", "unexpected_monitoring_service_cpu");
            announce();
            return;
        }
        if (policy.requests(Channel::temperature)) {
            auto row = empty_channel(Channel::temperature);
            prepare_source(row, thermal_source, HwmonTemperatureSource::discover());
            temperature.unavailable = std::move(row.at("unavailable_reason"));
        }
        for (auto& [cpu, target] : frequency) {
            auto row = empty_channel(Channel::frequency);
            prepare_source(row, target.source, CpuFrequencyPolicySource::discover(cpu));
            target.series.unavailable = std::move(row.at("unavailable_reason"));
        }
        announce();
        std::unique_lock lock{mutex};
        while (!token.stop_requested()) {
            if (!wake.wait(lock, token, [&] { return active.load(std::memory_order_acquire) != 0; }))
                break;
            lock.unlock();
            if (thermal_source && temperature.unavailable.is_null())
                temperature.append([&] { return thermal_source->read(); });
            for (auto& [cpu, target] : frequency) {
                if (target.active.load(std::memory_order_acquire) && target.source &&
                    target.series.unavailable.is_null())
                    target.series.append([&] { return read_frequency(*target.source); });
            }
            lock.lock();
            wake.wait_for(lock, token, std::chrono::nanoseconds{static_cast<std::int64_t>(policy.interval_ns)},
                          [] { return false; });
        }
    }

    void open_window(LinuxCpuId cpu) {
        if (const auto found = frequency.find(cpu); found != frequency.end())
            found->second.active.fetch_add(1U, std::memory_order_release);
        // The shared mutex prevents the initial notification from passing a
        // waiting service between its predicate check and entering the wait.
        std::lock_guard lock{mutex};
        if (active.fetch_add(1U, std::memory_order_release) == 0U)
            wake.notify_one();
    }

    void close_window(LinuxCpuId cpu) noexcept {
        if (const auto found = frequency.find(cpu); found != frequency.end())
            found->second.active.fetch_sub(1U, std::memory_order_release);
        active.fetch_sub(1U, std::memory_order_release);
    }
};

PackageMonitor::PackageMonitor(monitor::Policy policy, std::span<const LinuxCpuId> cpus, bool supplied)
    : state_{std::make_unique<State>(policy, supplied)} {
    if (supplied || !policy.during || !policy.service_cpu ||
        (!policy.requests(Channel::temperature) && !policy.requests(Channel::frequency)))
        return;
    if (policy.requests(Channel::frequency))
        for (const auto cpu : cpus) state_->frequency.try_emplace(cpu);
    state_->service = std::jthread([state = state_.get()](std::stop_token token) {
        try {
            state->run(token);
        } catch (const std::exception&) {
            state->service_error = reason("capture_failed", "monitoring_service_failed");
            state->announce();
        }
    });
    state_->ready.wait();
}

PackageMonitor::~PackageMonitor() { stop(); }

void PackageMonitor::stop() noexcept {
    if (state_->service.joinable()) {
        state_->service.request_stop();
        state_->service.join();
    }
}

void PackageMonitor::complete(Json& retained, LinuxCpuId cpu, U64 begin, U64 end) const {
    if (state_->supplied || !state_->policy.during || !state_->policy.service_cpu || retained.is_null() ||
        end <= begin)
        return;
    for (const auto channel : {Channel::temperature, Channel::frequency}) {
        if (!state_->policy.requests(channel)) continue;
        auto& row = retained.at(monitor::channel_name(channel));
        if (!state_->service_error.is_null()) {
            retain_reason(row, state_->service_error);
            continue;
        }
        const auto& series = channel == Channel::temperature ? state_->temperature
                                                             : state_->frequency.at(cpu).series;
        for (const auto& point : series.points) {
            if (point.at("read_start_ns").get<U64>() < begin || point.at("time_ns").get<U64>() > end) continue;
            if (state_->retained_points >= max_retained_poll_samples) {
                retain_reason(row, reason("capture_failed", "during_observation_result_capacity"));
                break;
            }
            row.at("during").push_back(point);
            ++state_->retained_points;
        }
        if (!series.unavailable.is_null())
            retain_reason(row, series.unavailable);
        if (row.at("during").empty() && row.at("unavailable_reason").is_null()) {
            row.at("during_unavailable_reason") = end - begin < state_->policy.interval_ns
                ? "window_shorter_than_sampling_interval" : "no_in_window_samples";
            if (!state_->policy.before && !state_->policy.after)
                retain_reason(row, reason("missing", "no_in_window_samples"));
        }
    }
}

struct PackageMonitorClock::State {
    PackageMonitor::State& owner;
    const LinuxCpuId cpu;
    Json& retained;
    std::optional<PerfContextSwitchSource> context;
    std::optional<HwmonTemperatureSource> temperature;
    std::optional<CpuFrequencyPolicySource> frequency;
    bool first{true}, opened{};

    State(PackageMonitor::State& state, LinuxCpuId requested, Json& evidence)
        : owner{state}, cpu{requested}, retained{evidence} {}

    auto row(Channel channel) -> Json& { return retained.at(monitor::channel_name(channel)); }

    void prepare(const Json* fixture) {
        retained = Json::object();
        for (std::size_t i = 0; i < monitor::channel_names.size(); ++i) {
            const auto channel = static_cast<Channel>(i);
            if (!owner.policy.requests(channel)) continue;
            auto raw = empty_channel(channel);
            const auto unsupported = monitor::unsupported_sampling(channel, owner.policy);
            if (!unsupported.empty()) raw.at("unavailable_reason") = reason("unsupported", unsupported);
            else if (owner.supplied) raw.at("unavailable_reason") = reason("unsupported", "supplied_reading_missing");
            retained[monitor::channel_name(channel)] = std::move(raw);
        }
        if (owner.supplied) {
            if (fixture) {
                if (!fixture->is_object()) throw std::invalid_argument("Expected supplied channels by name");
                retained.update(*fixture);
            }
            return;
        }
        if (owner.policy.requests(Channel::context_switches)) {
            auto thread = current_thread_id();
            if (!thread) {
                retain_reason(row(Channel::context_switches), reason(thread.error()));
            } else {
                auto source = PerfContextSwitchSource::prepare(*thread);
                if (source) context.emplace(std::move(*source));
                else retain_reason(row(Channel::context_switches), reason(source.error(), true));
            }
        }
        if (owner.policy.before || owner.policy.after) {
            if (owner.policy.requests(Channel::temperature))
                prepare_source(row(Channel::temperature), temperature, HwmonTemperatureSource::discover());
            if (owner.policy.requests(Channel::frequency))
                prepare_source(row(Channel::frequency), frequency, CpuFrequencyPolicySource::discover(cpu));
        }
    }

    void workers(const char* phase) {
        if (owner.policy.requests(Channel::page_faults) && owner.policy.before && owner.policy.after) {
            const auto op = detail::production_thread_rusage_operations();
            const auto faults = detail::ThreadRusageAccess::read_page_fault_counters(op);
            retain_sample(row(Channel::page_faults), phase, faults.transform([](auto value) {
                return Json{{"minor", value.minor_faults}, {"major", value.major_faults}};
            }));
        }
        if (owner.policy.requests(Channel::cpu_migration) && owner.policy.before && owner.policy.after)
            retain_sample(row(Channel::cpu_migration), phase, current_cpu().transform([](auto value) { return value.value(); }));
    }

    void sensors(const char* phase) {
        if (temperature) retain_sample(row(Channel::temperature), phase, temperature->read());
        if (frequency) retain_sample(row(Channel::frequency), phase, read_frequency(*frequency));
    }

    void drain() {
        if (!context) return;
        auto& raw = row(Channel::context_switches);
        auto events = context->drain();
        auto time = raw_now();
        if (!events || !time) {
            retain_reason(raw, reason(events ? time.error() : events.error()));
            return;
        }
        raw.at("drained_ns") = *time;
        if (events->completeness() != PerfCaptureCompleteness::complete)
            retain_reason(raw, reason("capture_failed", "context_switch_capture_incomplete"));
        for (const auto& event : events->records()) {
            constexpr std::array<std::string_view, 3> kinds{"switch_in", "voluntary_switch_out", "involuntary_switch_out"};
            raw.at("events").push_back({{"time_ns", event.timestamp().nanoseconds()},
                                        {"kind", kinds.at(static_cast<std::size_t>(event.kind()))}});
        }
    }

    void begin() {
        if (owner.policy.before) sensors("before");
        if (owner.service.joinable()) {
            owner.open_window(cpu);
            opened = true;
        }
        workers("before");
    }

    void end() {
        workers("after");
        if (opened) {
            owner.close_window(cpu);
            opened = false;
        }
        drain();
        if (owner.policy.after) sensors("after");
    }
};

PackageMonitorClock::PackageMonitorClock(Clock clock, PackageMonitor* monitor, LinuxCpuId cpu,
                                         const Json* supplied, Json& retained)
    : clock_{clock} {
    if (!monitor) return;
    state_ = std::make_unique<State>(*monitor->state_, cpu, retained);
    state_->prepare(supplied);
}

PackageMonitorClock::~PackageMonitorClock() {
    if (state_ && state_->opened) state_->owner.close_window(state_->cpu);
}

auto PackageMonitorClock::clock() noexcept -> Clock {
    if (!state_ || state_->owner.supplied) return clock_;
    return {this, now};
}

void PackageMonitorClock::begin_bracket() {
    if (state_ && !state_->owner.supplied) state_->begin();
}

void PackageMonitorClock::end_bracket() {
    if (state_ && !state_->owner.supplied) state_->end();
}

auto PackageMonitorClock::now(void* pointer) -> std::expected<U64, core::Error> {
    auto& self = *static_cast<PackageMonitorClock*>(pointer);
    auto& state = *self.state_;
    const bool first = std::exchange(state.first, false);
    if (first) state.begin();
    const auto raw = self.clock_.now ? self.clock_.now(self.clock_.context) : raw_now();
    if (!first) state.end();
    return raw;
}

} // namespace epycbench::platform_linux::package_execution
