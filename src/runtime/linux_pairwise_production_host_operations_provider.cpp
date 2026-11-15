#include "epycbench/runtime/linux_pairwise_production_host_operations_provider.hpp"
#include "runtime/linux_pairwise_production_host_operations_provider_access.hpp"
#include "runtime/pairwise_production_operations_adapter_access.hpp"
#include "platform_linux/numa_operations.hpp"

#include "epycbench/monitoring/linux_driver_operations_provider.hpp"
#include "epycbench/platform_linux/memory_mapping.hpp"
#include "epycbench/platform_linux/numa_policy.hpp"
#include "epycbench/platform_linux/page_location.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"
#include "memory/region_initialization_access.hpp"
#include "runtime/worker_team_state.hpp"

#include <utility>

namespace epycbench::runtime {
namespace {

using Host = PairwiseProductionHostOperations;

auto now(void*) noexcept -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    return platform_linux::raw_monotonic_now();
}
auto tid(void*) -> std::expected<platform_linux::LinuxThreadId, core::Error> {
    return platform_linux::current_thread_id();
}
auto set_affinity(void*, const core::CpuSet& cpus) -> std::expected<void, core::Error> {
    return platform_linux::set_current_thread_affinity(cpus);
}
auto get_affinity(void*) -> std::expected<core::CpuSet, core::Error> {
    return platform_linux::get_current_thread_affinity();
}
auto cpu(void*) -> std::expected<core::LinuxCpuId, core::Error> {
    return platform_linux::current_cpu();
}
auto launch(void*, void (*entry)(void*, std::size_t) noexcept, void* state,
            std::size_t index) -> std::expected<std::jthread, core::Error> {
    const auto launcher = detail::production_worker_thread_launcher();
    return launcher.launch(launcher.context, entry, state, index);
}
auto map(void*, std::size_t bytes)
    -> std::expected<platform_linux::AnonymousMapping, core::Error> {
    return platform_linux::AnonymousMapping::create(bytes);
}
auto mask(void*, const core::LinuxNumaNodeSet& nodes)
    -> std::expected<platform_linux::NativeNumaMask, core::Error> {
    return platform_linux::NativeNumaMask::from_node_set(nodes);
}
auto bind(void*, const platform_linux::AnonymousMapping& mapping,
          const platform_linux::NativeNumaMask& nodes) -> std::expected<void, core::Error> {
    return platform_linux::bind_memory(mapping, nodes);
}
auto prefault(void*, std::span<std::byte> bytes, std::size_t offset, std::byte value)
    -> std::expected<void, core::Error> {
    const auto operations = memory::detail::production_region_initialization_operations();
    return operations.write_prefault_byte(operations.context, bytes, offset, value);
}
auto query(void*, std::span<void* const> pages)
    -> std::expected<std::vector<platform_linux::PageQueryStatus>, core::Error> {
    return platform_linux::query_page_locations(pages);
}
auto protect(void*, platform_linux::AnonymousMapping& mapping)
    -> std::expected<void, core::Error> {
    return mapping.protect(platform_linux::MemoryProtection::read_only);
}

auto production_host(monitoring::MonitoringDriverOperations monitoring) -> Host {
    return {.context = {},
            .clock = {.context = nullptr, .now = now},
            .current_thread_id = tid,
            .set_current_thread_affinity = set_affinity,
            .get_current_thread_affinity = get_affinity,
            .current_cpu = cpu,
            .launch_worker = launch,
            .map = map,
            .numa_mask = mask,
            .bind = bind,
            .write_prefault_byte = prefault,
            .query_pages = query,
            .protect_read_only = protect,
            .monitoring = monitoring};
}

} // namespace

struct LinuxPairwiseProductionHostOperationsProvider::Context {
    std::unique_ptr<monitoring::LinuxMonitoringDriverOperationsProvider> monitoring_provider;
    std::shared_ptr<void> test_monitoring_owner;
    std::optional<platform_linux::detail::NumaOperations> test_diagnostic_numa;
    Host host;
};

LinuxPairwiseProductionHostOperationsProvider::LinuxPairwiseProductionHostOperationsProvider(
    std::shared_ptr<Context> context) noexcept : context_{std::move(context)} {}
LinuxPairwiseProductionHostOperationsProvider::LinuxPairwiseProductionHostOperationsProvider(
    LinuxPairwiseProductionHostOperationsProvider&&) noexcept = default;
LinuxPairwiseProductionHostOperationsProvider::~LinuxPairwiseProductionHostOperationsProvider() noexcept = default;

auto LinuxPairwiseProductionHostOperationsProvider::operations() const noexcept -> Host {
    return context_->host;
}
auto LinuxPairwiseProductionHostOperationsProvider::adapter() const
    -> PairwiseProductionOperationsAdapter {
    return detail::PairwiseProductionOperationsAdapterAccess::create_linux(
        context_->host,
        context_->test_diagnostic_numa.value_or(
            platform_linux::detail::production_numa_operations()),
        context_);
}

auto make_linux_pairwise_production_host_operations_provider()
    -> LinuxPairwiseProductionHostOperationsProvider {
    auto monitoring_provider = std::make_unique<monitoring::LinuxMonitoringDriverOperationsProvider>(
        monitoring::make_linux_monitoring_driver_operations_provider());
    auto context = std::make_shared<LinuxPairwiseProductionHostOperationsProvider::Context>();
    context->host = production_host(monitoring_provider->operations());
    context->monitoring_provider = std::move(monitoring_provider);
    return LinuxPairwiseProductionHostOperationsProvider{std::move(context)};
}

auto detail::LinuxPairwiseProductionHostOperationsProviderAccess::create(
    Dependencies dependencies) -> LinuxPairwiseProductionHostOperationsProvider {
    auto context = std::make_shared<LinuxPairwiseProductionHostOperationsProvider::Context>();
    context->host = std::move(dependencies.host);
    context->test_monitoring_owner = std::move(dependencies.monitoring_owner);
    context->test_diagnostic_numa = dependencies.diagnostic_numa;
    return LinuxPairwiseProductionHostOperationsProvider{std::move(context)};
}

} // namespace epycbench::runtime
