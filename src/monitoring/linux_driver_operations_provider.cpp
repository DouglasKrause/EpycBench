#include "epycbench/monitoring/linux_driver_operations_provider.hpp"
#include "linux_driver_operations_provider_access.hpp"

#include "epycbench/monitoring/linux_interruptible_scheduler.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace epycbench::monitoring {
namespace {

auto provider_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.linux_provider.invalid_state"},
            .message = "Linux monitoring provider callback used without prepared source state",
            .operation = "linux_monitoring_driver_operations_provider",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto contained_error(std::string classification) -> core::Error {
    auto value = provider_error(std::move(classification));
    value.code = core::StableErrorCode{"monitoring.linux_provider.contained_failure"};
    value.message = "Linux monitoring provider callback failure was contained";
    return value;
}

std::atomic injected_boundary{detail::LinuxProviderReadBoundary::none};
std::atomic injected_kind{detail::LinuxProviderExceptionKind::standard};
void inject_if(detail::LinuxProviderReadBoundary boundary) {
    auto expected = boundary;
    if (!injected_boundary.compare_exchange_strong(
            expected, detail::LinuxProviderReadBoundary::none)) return;
    if (injected_kind.load() == detail::LinuxProviderExceptionKind::standard)
        throw std::runtime_error{"injected"};
    throw 1;
}

constexpr auto channel_index(MonitoringChannel channel) noexcept -> std::size_t {
    switch (channel) {
    case MonitoringChannel::page_faults: return 0;
    case MonitoringChannel::context_switches: return 1;
    case MonitoringChannel::temperature: return 2;
    case MonitoringChannel::frequency: return 3;
    case MonitoringChannel::throttling: return 4;
    }
    return 4;
}

template <class Payload>
auto read_payload(MonitoringSourceOperations<Payload> operations, std::string classification)
    -> std::expected<Payload, core::Error> {
    auto value = operations.read();
    if (!value) return std::unexpected{std::move(value.error())};
    if (value->status() != MonitoringAcquisitionStatus::available || !value->payload())
        return std::unexpected{provider_error(std::move(classification))};
    return *value->payload();
}

auto production_bind(void*, core::LinuxCpuId cpu) noexcept
    -> std::expected<void, core::Error> {
    return platform_linux::set_current_thread_affinity(core::CpuSet{cpu});
}
auto production_now(void*) noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    return platform_linux::raw_monotonic_now();
}
auto production_page(void*, core::WorkerId worker,
                     platform_linux::LinuxThreadId tid) noexcept -> PageFaultSource {
    return PageFaultSource::create(worker, tid);
}
auto production_context(void*, core::WorkerId worker, platform_linux::LinuxThreadId tid) noexcept
    -> std::expected<ContextSwitchEventSource, core::Error> {
    return ContextSwitchEventSource::prepare(worker, tid);
}
auto production_temperature(void*) noexcept
    -> std::expected<std::optional<TemperatureSource>, core::Error> {
    return TemperatureSource::prepare();
}
auto production_frequency(void*, core::LinuxCpuId cpu) noexcept
    -> std::expected<std::optional<FrequencySource>, core::Error> {
    return FrequencySource::prepare(cpu);
}

} // namespace

struct LinuxMonitoringDriverOperationsProvider::Context {
    struct WorkerPage { core::WorkerId worker; platform_linux::LinuxThreadId tid; PageFaultSource source; };
    struct WorkerContext { core::WorkerId worker; platform_linux::LinuxThreadId tid; ContextSwitchEventSource source; };
    struct PreparedGeneration {
        std::vector<WorkerPage> pages;
        std::vector<WorkerContext> contexts;
        std::optional<TemperatureSource> temperature;
        std::vector<FrequencySource> frequencies;
        std::array<MonitoringSourceSupport, 4> support{
            MonitoringSourceSupport::unsupported, MonitoringSourceSupport::unsupported,
            MonitoringSourceSupport::unsupported, MonitoringSourceSupport::unsupported};
    };

    LinuxInterruptibleRawMonotonicScheduler scheduler;
    Dependencies dependencies;
    std::optional<PreparedGeneration> prepared;
};

namespace {
using Provider = LinuxMonitoringDriverOperationsProvider;

auto prepare(void* opaque, const MonitoringSourcePreparationRequest& request) noexcept
    -> std::expected<MonitoringPreparedSources, core::Error> {
    auto& context = *static_cast<Provider::Context*>(opaque);
    try {
        Provider::Context::PreparedGeneration candidate;
        const auto requested = [&](MonitoringChannel channel) {
            return request.policy().disposition(channel) != MonitoringChannelDisposition::disabled;
        };
        // Deterministic internal order; only atomic publication is externally observable.
        if (requested(MonitoringChannel::page_faults)) {
            candidate.pages.reserve(request.workers().size());
            for (const auto& worker : request.workers())
                candidate.pages.push_back({worker.worker_id(), worker.operational_thread_id(),
                    context.dependencies.page(context.dependencies.context, worker.worker_id(),
                                              worker.operational_thread_id())});
            candidate.support[0] = MonitoringSourceSupport::supported;
        }
        if (requested(MonitoringChannel::context_switches)) {
            candidate.contexts.reserve(request.workers().size());
            for (const auto& worker : request.workers()) {
                auto source = context.dependencies.context_switch(
                    context.dependencies.context, worker.worker_id(),
                    worker.operational_thread_id());
                if (!source) return std::unexpected{std::move(source.error())};
                candidate.contexts.push_back({worker.worker_id(), worker.operational_thread_id(),
                                              std::move(*source)});
            }
            candidate.support[1] = MonitoringSourceSupport::supported;
        }
        if (requested(MonitoringChannel::temperature)) {
            auto source = context.dependencies.temperature(context.dependencies.context);
            if (!source) return std::unexpected{std::move(source.error())};
            candidate.temperature = std::move(*source);
            candidate.support[2] = candidate.temperature ? MonitoringSourceSupport::supported
                                                         : MonitoringSourceSupport::unsupported;
        }
        if (requested(MonitoringChannel::frequency)) {
            for (const auto& worker : request.workers()) {
                const auto already_covered = std::ranges::any_of(candidate.frequencies,
                    [&](const auto& source) {
                        return std::ranges::find(source.metadata().domain_id().cpus(),
                                                 worker.linux_cpu()) !=
                               source.metadata().domain_id().cpus().end();
                    });
                if (already_covered) continue;
                auto source = context.dependencies.frequency(context.dependencies.context,
                                                             worker.linux_cpu());
                if (!source) return std::unexpected{std::move(source.error())};
                if (*source) candidate.frequencies.push_back(std::move(**source));
            }
            std::ranges::sort(candidate.frequencies, {}, [](const auto& source) -> const auto& {
                return source.metadata().domain_id();
            });
            candidate.support[3] = candidate.frequencies.empty()
                                       ? MonitoringSourceSupport::unsupported
                                       : MonitoringSourceSupport::supported;
        }
        MonitoringPreparedSources result;
        result.frequency_domains.reserve(candidate.frequencies.size());
        for (const auto& source : candidate.frequencies)
            result.frequency_domains.push_back(
                {source.metadata(), source.implementation_id()});
        context.prepared = std::move(candidate);
        return result;
    } catch (const std::exception&) {
        return std::unexpected{provider_error("contained_exception")};
    } catch (...) {
        return std::unexpected{provider_error("contained_unknown_failure")};
    }
}

auto bind(void* opaque, core::LinuxCpuId cpu) noexcept -> std::expected<void, core::Error> {
    auto& context = *static_cast<Provider::Context*>(opaque);
    return context.dependencies.bind(context.dependencies.context, cpu);
}
auto now(void* opaque) noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    auto& context = *static_cast<Provider::Context*>(opaque);
    return context.dependencies.now(context.dependencies.context);
}
auto wait(void* opaque, platform_linux::RawMonotonicTimestamp deadline) noexcept
    -> std::expected<MonitoringSchedulerWaitResult, core::Error> {
    auto& context = *static_cast<Provider::Context*>(opaque);
    if (context.dependencies.wait)
        return context.dependencies.wait(context.dependencies.context, deadline);
    return context.scheduler.wait_until(deadline);
}
void wake(void* opaque) noexcept {
    auto& context = *static_cast<Provider::Context*>(opaque);
    if (context.dependencies.wake) context.dependencies.wake(context.dependencies.context);
    else context.scheduler.wake();
}
auto support(void* opaque, MonitoringChannel channel) noexcept -> MonitoringSourceSupport {
    const auto& context = *static_cast<Provider::Context*>(opaque);
    const auto index = channel_index(channel);
    return context.prepared && index < 4 ? context.prepared->support[index]
                                         : MonitoringSourceSupport::unsupported;
}

template <class Entry>
auto find_worker(std::vector<Entry>& entries, core::WorkerId worker,
                 platform_linux::LinuxThreadId tid) -> Entry* {
    const auto found = std::ranges::find_if(entries, [&](const auto& entry) {
        return entry.worker == worker && entry.tid == tid;
    });
    return found == entries.end() ? nullptr : &*found;
}
auto read_page(void* opaque, core::WorkerId worker, platform_linux::LinuxThreadId tid) noexcept
    -> std::expected<PageFaultSnapshot, core::Error> {
    try {
    inject_if(detail::LinuxProviderReadBoundary::page);
    auto& context = *static_cast<Provider::Context*>(opaque);
    auto* entry = context.prepared ? find_worker(context.prepared->pages, worker, tid) : nullptr;
    if (!entry) return std::unexpected{provider_error("page_fault_unprepared")};
    auto operations = entry->source.operations();
    if (!operations) return std::unexpected{std::move(operations.error())};
    return read_payload(*operations, "page_fault_unavailable");
    } catch (const std::exception&) { return std::unexpected{contained_error("contained_exception")}; }
    catch (...) { return std::unexpected{contained_error("contained_unknown_failure")}; }
}
auto drain_context(void* opaque, core::WorkerId worker,
                   platform_linux::LinuxThreadId tid) noexcept
    -> std::expected<ContextSwitchEventBatch, core::Error> {
    try {
    inject_if(detail::LinuxProviderReadBoundary::context);
    auto& context = *static_cast<Provider::Context*>(opaque);
    auto* entry = context.prepared ? find_worker(context.prepared->contexts, worker, tid) : nullptr;
    if (!entry) return std::unexpected{provider_error("context_switch_unprepared")};
    auto operations = entry->source.operations();
    if (!operations) return std::unexpected{std::move(operations.error())};
    return read_payload(*operations, "context_switch_unavailable");
    } catch (const std::exception&) { return std::unexpected{contained_error("contained_exception")}; }
    catch (...) { return std::unexpected{contained_error("contained_unknown_failure")}; }
}
auto read_temperature(void* opaque) noexcept
    -> std::expected<TemperatureSnapshot, core::Error> {
    try {
    inject_if(detail::LinuxProviderReadBoundary::temperature);
    auto& context = *static_cast<Provider::Context*>(opaque);
    if (!context.prepared || !context.prepared->temperature)
        return std::unexpected{provider_error("temperature_unprepared")};
    auto operations = context.prepared->temperature->operations();
    if (!operations) return std::unexpected{std::move(operations.error())};
    return read_payload(*operations, "temperature_unavailable");
    } catch (const std::exception&) { return std::unexpected{contained_error("contained_exception")}; }
    catch (...) { return std::unexpected{contained_error("contained_unknown_failure")}; }
}
auto read_frequency(void* opaque, const FrequencyDomainId& domain) noexcept
    -> std::expected<FrequencySnapshot, core::Error> {
    try {
    inject_if(detail::LinuxProviderReadBoundary::frequency);
    auto& context = *static_cast<Provider::Context*>(opaque);
    if (!context.prepared) return std::unexpected{provider_error("frequency_unprepared")};
    const auto found = std::ranges::find_if(context.prepared->frequencies,
        [&](const auto& source) { return source.metadata().domain_id() == domain; });
    if (found == context.prepared->frequencies.end())
        return std::unexpected{provider_error("frequency_domain_unprepared")};
    auto operations = found->operations();
    if (!operations) return std::unexpected{std::move(operations.error())};
    return read_payload(*operations, "frequency_unavailable");
    } catch (const std::exception&) { return std::unexpected{contained_error("contained_exception")}; }
    catch (...) { return std::unexpected{contained_error("contained_unknown_failure")}; }
}
} // namespace

LinuxMonitoringDriverOperationsProvider::LinuxMonitoringDriverOperationsProvider(
    std::unique_ptr<Context> context) noexcept : context_{std::move(context)} {}
LinuxMonitoringDriverOperationsProvider::LinuxMonitoringDriverOperationsProvider(
    LinuxMonitoringDriverOperationsProvider&&) noexcept = default;
LinuxMonitoringDriverOperationsProvider::~LinuxMonitoringDriverOperationsProvider() noexcept = default;

auto LinuxMonitoringDriverOperationsProvider::operations() noexcept
    -> MonitoringDriverOperations {
    return {.context = context_.get(), .prepare_sources = prepare, .bind_service_cpu = bind,
            .now = now, .wait_until = wait, .wake = wake, .support = support,
            .read_page_fault = read_page, .drain_context_switch = drain_context,
            .read_temperature = read_temperature, .read_frequency = read_frequency};
}

auto make_linux_monitoring_driver_operations_provider()
    -> LinuxMonitoringDriverOperationsProvider {
    auto context = std::make_unique<LinuxMonitoringDriverOperationsProvider::Context>();
    context->dependencies = {.context = nullptr, .bind = production_bind, .now = production_now,
                             .wait = nullptr, .wake = nullptr, .page = production_page,
                             .context_switch = production_context,
                             .temperature = production_temperature,
                             .frequency = production_frequency};
    return LinuxMonitoringDriverOperationsProvider{std::move(context)};
}

auto detail::LinuxMonitoringDriverOperationsProviderAccess::create(Dependencies dependencies)
    -> LinuxMonitoringDriverOperationsProvider {
    auto context = std::make_unique<LinuxMonitoringDriverOperationsProvider::Context>();
    context->dependencies = dependencies;
    return LinuxMonitoringDriverOperationsProvider{std::move(context)};
}

void detail::LinuxMonitoringDriverOperationsProviderAccess::inject_read_exception(
    LinuxProviderReadBoundary boundary, LinuxProviderExceptionKind kind) noexcept {
    injected_kind.store(kind); injected_boundary.store(boundary);
}
void detail::LinuxMonitoringDriverOperationsProviderAccess::clear_read_exception() noexcept {
    injected_boundary.store(LinuxProviderReadBoundary::none);
}

} // namespace epycbench::monitoring
