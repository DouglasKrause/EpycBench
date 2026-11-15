#pragma once
#include "monitoring/host_q4_kernel_maximum_authority.hpp"
#include "platform_linux/authenticated_monitoring_capacity_authority_test_access.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4KernelMaximumAuthorityTestAccess final {
  private:
    static auto prepared(const MonitoringQ4KernelMaximumReady* ready = nullptr)
        -> platform_linux::detail::PreparedLinuxMonitoringCapacityAuthority {
        const auto& projection = ready ? ready->predecessor_projection()
                                       : MonitoringQ4AuthenticatedHostProjection{};
        auto physical = ready
                            ? *topology::PhysicalTopologyFingerprint::parse(
                                  projection.physical_fingerprint)
                            : *topology::PhysicalTopologyFingerprint::parse(
                                  "sha256:" + core::sha256("physical").to_hex());
        auto effective = ready
                             ? *topology::EffectiveTopologyFingerprint::parse(
                                   projection.effective_fingerprint)
                             : *topology::EffectiveTopologyFingerprint::parse(
                                   "sha256:" + core::sha256("effective").to_hex());
        using CapacityAccess =
            platform_linux::detail::LinuxMonitoringCapacityAuthorityTestAccess;
        auto provenance = CapacityAccess::host_plan_provenance(
            core::sha256("environment"), core::sha256("plan"));
        std::vector<core::LinuxCpuId> cpus;
        if (ready)
            for (const auto cpu : projection.admitted_linux_cpus)
                cpus.emplace_back(cpu.value);
        else
            cpus.emplace_back(0);
        auto facts = CapacityAccess::host_facts(
            core::ByteCount::from_base_units(
                ready ? projection.page_size_bytes.value : 4096), std::move(cpus),
            std::move(physical), std::move(effective), std::move(provenance));
        return std::move(*CapacityAccess::prepare(std::move(facts)));
    }
  public:
    using Operations = MonitoringQ4KernelMaximumAuthority::Operations;
    static auto retain(Operations operations) noexcept {
        auto value = MonitoringQ4KernelMaximumAuthority::retain_with(operations);
        if (value)
            value->prepared_.emplace(prepared());
        return value;
    }
    static auto retain(Operations operations,
                       const MonitoringQ4KernelMaximumReady& ready) noexcept {
        auto value = MonitoringQ4KernelMaximumAuthority::retain_with(operations);
        if (value) {
            value->binding_ = ready.kernel_maximum_binding_;
            value->prepared_.emplace(prepared(&ready));
        }
        return value;
    }
    static auto value(const MonitoringQ4AuthenticatedKernelMaximum& value) noexcept {
        return value.value_;
    }
};
} // namespace epycbench::monitoring::qualification::detail
