#pragma once

#include "platform_linux/authenticated_monitoring_capacity_authority.hpp"

namespace epycbench::platform_linux::detail {

struct LinuxMonitoringCapacityAuthorityTestAccess final {
    using PreparationOperations = LinuxMonitoringCapacityAuthorityConstruction::PreparationOperations;
    using Operations = AuthenticatedLinuxMonitoringCapacityAuthority::Operations;
    [[nodiscard]] static auto scenario_provenance(
        const core::Sha256Digest& first, const core::Sha256Digest& second) {
        return LinuxMonitoringCapacityAuthorityConstruction::scenario_provenance(first, second);
    }
    [[nodiscard]] static auto host_plan_provenance(
        const core::Sha256Digest& first, const core::Sha256Digest& second) {
        return LinuxMonitoringCapacityAuthorityConstruction::host_plan_provenance(first, second);
    }
    [[nodiscard]] static auto host_facts(
        core::ByteCount page, std::vector<core::LinuxCpuId> cpus,
        topology::PhysicalTopologyFingerprint physical,
        topology::EffectiveTopologyFingerprint effective,
        LinuxMonitoringCapacityProvenance provenance) noexcept {
        return LinuxMonitoringCapacityAuthorityConstruction::host_facts(
            page, std::move(cpus), std::move(physical), std::move(effective),
            std::move(provenance));
    }
    [[nodiscard]] static auto prepare(LinuxMonitoringCapacityHostFacts&& facts) noexcept {
        return LinuxMonitoringCapacityAuthorityConstruction::prepare(std::move(facts));
    }
    [[nodiscard]] static auto prepare_with(LinuxMonitoringCapacityHostFacts&& facts,
                                           PreparationOperations operations) noexcept {
        return LinuxMonitoringCapacityAuthorityConstruction::prepare_with(
            std::move(facts), operations);
    }
    [[nodiscard]] static auto seal(PreparedLinuxMonitoringCapacityAuthority&& prepared,
                                   std::uint32_t maximum) noexcept {
        return LinuxMonitoringCapacityAuthorityConstruction::seal(std::move(prepared), maximum);
    }
    [[nodiscard]] static auto authenticate(LinuxMonitoringCapacityHostFacts&& facts,
                                           Operations operations) noexcept {
        return LinuxMonitoringCapacityAuthorityConstruction::authenticate_with(
            std::move(facts), operations);
    }
    static void invalidate_binding(AuthenticatedLinuxMonitoringCapacityAuthority& value) noexcept {
        value.binding_.reset();
    }
};

} // namespace epycbench::platform_linux::detail
