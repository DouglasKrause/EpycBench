#include "monitoring/host_q4_monitoring_capacity_adapter.hpp"

#include <algorithm>
#include <ranges>

namespace epycbench::monitoring::qualification::detail {
namespace {
using Reason = MonitoringQ4CapacityReason;
auto failure(MonitoringQ4KernelMaximumFailureKind kind, Reason reason = Reason::none) noexcept {
    return MonitoringQ4KernelMaximumFailure{kind, 0, false, std::nullopt, reason};
}
} // namespace

auto monitoring_q4_linux_capacity_reason(platform_linux::detail::LinuxMonitoringCapacityReason reason) noexcept
    -> MonitoringQ4CapacityReason {
    using L = platform_linux::detail::LinuxMonitoringCapacityReason;
    switch (reason) {
    case L::prepare_binding_exception: return Reason::linux_prepare_binding_exception;
    case L::prepare_binding_unknown: return Reason::linux_prepare_binding_unknown;
    case L::prepare_cpus_duplicate: return Reason::linux_prepare_cpus_duplicate;
    case L::prepare_cpus_empty: return Reason::linux_prepare_cpus_empty;
    case L::prepare_cpus_order: return Reason::linux_prepare_cpus_order;
    case L::prepare_page_zero: return Reason::linux_prepare_page_zero;
    case L::prepare_preimage_exception: return Reason::linux_prepare_preimage_exception;
    case L::prepare_preimage_unknown: return Reason::linux_prepare_preimage_unknown;
    case L::prepare_preimage_size: return Reason::linux_prepare_preimage_size;
    case L::seal_cpu_range: return Reason::linux_seal_cpu_range;
    case L::seal_hash_allocation: return Reason::linux_seal_hash_allocation;
    case L::seal_hash_exception: return Reason::linux_seal_hash_exception;
    case L::seal_hash_unknown: return Reason::linux_seal_hash_unknown;
    case L::seal_preimage_allocation: return Reason::linux_seal_preimage_allocation;
    case L::seal_preimage_exception: return Reason::linux_seal_preimage_exception;
    case L::seal_preimage_unknown: return Reason::linux_seal_preimage_unknown;
    case L::none: return Reason::none;
    }
    return Reason::none;
}

auto MonitoringQ4MonitoringCapacityAdapter::prepare(
    const MonitoringQ4KernelMaximumReady& ready) noexcept
    -> std::expected<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority,
                     MonitoringQ4KernelMaximumFailure> {
    try {
        auto provider = runtime::production_runner_context_provider();
        if (provider.acquire == nullptr)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::contained)};
        auto context = provider.acquire(provider.context);
        if (!context)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::contained)};
        return prepare_with(ready, *context);
    } catch (...) {
        return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::contained)};
    }
}

auto MonitoringQ4MonitoringCapacityAdapter::prepare_with(
    const MonitoringQ4KernelMaximumReady& ready, const runtime::RunnerContext& context) noexcept
    -> std::expected<platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority,
                     MonitoringQ4KernelMaximumFailure> {
    try {
        const auto& projection = ready.predecessor_projection();
        const auto& machine = context.topology().machine();
        if (machine.page_sizes.empty())
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_pages_empty)};
        const auto page = std::ranges::min_element(machine.page_sizes);
        const auto& physical = projection.authenticated_physical_fingerprint;
        const auto& effective = projection.authenticated_effective_fingerprint;
        const auto& environment = projection.authenticated_host_environment_digest;
        const auto& plan = projection.authenticated_qualification_plan_digest;
        if (page == machine.page_sizes.end())
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_page_end)};
        if (page->value() == 0U)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_page_zero)};
        if (page->value() != projection.page_size_bytes.value)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_page_mismatch)};
        if (!physical)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_physical_missing)};
        if (!effective)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_effective_missing)};
        if (!environment)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_environment_missing)};
        if (!plan)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_plan_missing)};
        if (*physical != context.fingerprints().physical)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_physical_mismatch)};
        if (*effective != context.fingerprints().effective)
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_effective_mismatch)};
        if (projection.host_environment_digest != "sha256:" + environment->to_hex())
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_environment_text)};
        if (projection.qualification_plan_digest != "sha256:" + plan->to_hex())
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_plan_text)};
        if (projection.physical_fingerprint != physical->to_string())
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_physical_text)};
        if (projection.effective_fingerprint != effective->to_string())
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value, Reason::adapter_effective_text)};
        std::vector<core::LinuxCpuId> cpus;
        cpus.reserve(machine.present_cpus.cardinality());
        for (const auto cpu : machine.present_cpus)
            cpus.push_back(cpu);
        auto provenance =
            platform_linux::detail::LinuxMonitoringCapacityAuthorityConstruction::host_plan_provenance(
                *environment, *plan);
        auto facts = platform_linux::detail::LinuxMonitoringCapacityAuthorityConstruction::host_facts(
            *page, std::move(cpus), context.fingerprints().physical,
            context.fingerprints().effective, std::move(provenance));
        auto prepared =
            platform_linux::detail::LinuxMonitoringCapacityAuthorityConstruction::prepare(std::move(facts));
        if (!prepared) {
            if (prepared.error().primary == platform_linux::detail::LinuxMonitoringCapacityFailureKind::allocation)
                return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::contained)};
            return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::invalid_value,
                                          monitoring_q4_linux_capacity_reason(prepared.error().reason))};
        }
        return std::move(*prepared);
    } catch (...) {
        return std::unexpected{failure(MonitoringQ4KernelMaximumFailureKind::contained)};
    }
}

auto MonitoringQ4MonitoringCapacityAdapter::seal(
    platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority&& prepared,
    std::uint32_t maximum) noexcept
    -> std::expected<platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority,
                     platform_linux::detail::LinuxMonitoringCapacityFailure> {
    return platform_linux::detail::LinuxMonitoringCapacityAuthorityConstruction::seal(
        std::move(prepared), maximum);
}

} // namespace epycbench::monitoring::qualification::detail
