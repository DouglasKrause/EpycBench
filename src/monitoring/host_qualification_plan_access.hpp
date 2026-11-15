#pragma once

#include "epycbench/monitoring/host_environment.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace epycbench::monitoring::qualification {

class MonitoringHostQualificationPlan;
namespace detail {
enum class HostFrozenCompositionStage : unsigned char;
}

class detail_HostSessionBinding final {
  public:
    detail_HostSessionBinding(topology::PhysicalTopologyFingerprint physical,
                              topology::EffectiveTopologyFingerprint effective)
        : physical_{std::move(physical)}, effective_{std::move(effective)} {}
  private:
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    std::string environment_digest_;
    friend class detail_HostQualificationPlanAccess;
};

struct detail_HostSelectedWorkerRequirement final {
    std::string family;
    std::uint32_t worker_id;
    topology::StableNumaNodeId stable_node;
    topology::StableCoreId stable_core;
    topology::StableProcessingUnitId stable_pu;
    core::LinuxCpuId cpu;
    core::LinuxNumaNodeId node;
};

struct detail_HostSelectedServiceRequirement final {
    std::string role;
    topology::StableCoreId stable_core;
    std::vector<topology::StableProcessingUnitId> stable_pus;
    std::vector<core::LinuxCpuId> sibling_cpus;
};

struct detail_HostSelectedResourceRequirements final {
    std::shared_ptr<const detail_HostSessionBinding> binding;
    std::string environment_digest;
    topology::PhysicalTopologyFingerprint physical;
    topology::EffectiveTopologyFingerprint effective;
    std::vector<detail_HostSelectedWorkerRequirement> workers;
    std::vector<detail_HostSelectedServiceRequirement> service;
};

class detail_HostQualificationPlanAccess final {
  public:
    static auto binding_fallible(const RunnerContextAcquisitionSession&)
        -> std::expected<std::shared_ptr<const detail_HostSessionBinding>, core::Error>;
    static auto bind_environment_fallible(const RunnerContextAcquisitionSession&, std::string_view)
        -> std::expected<void, core::Error>;
    static auto register_requirements_fallible(const RunnerContextAcquisitionSession&,
                                               detail_HostSelectedResourceRequirements,
                                               std::optional<core::Error>* = nullptr)
        -> std::expected<void, core::Error>;
    static auto requirements_fallible(const RunnerContextAcquisitionSession&)
        -> std::expected<std::reference_wrapper<const detail_HostSelectedResourceRequirements>,
                         core::Error>;
    static auto resolve_fallible(const RunnerContextAcquisitionSession&,
                                 const HostEnvironmentAuthority&,
                                 std::optional<core::Error>* = nullptr,
                                 detail::HostFrozenCompositionStage* = nullptr)
        -> std::expected<MonitoringHostQualificationPlan, core::Error>;
    static auto binding(const RunnerContextAcquisitionSession&) noexcept
        -> std::expected<std::shared_ptr<const detail_HostSessionBinding>, core::Error>;
    static auto binding(const HostEnvironmentAuthority&) noexcept
        -> const std::shared_ptr<const detail_HostSessionBinding>&;
    static auto environment_digest(const std::shared_ptr<const detail_HostSessionBinding>&) noexcept
        -> const std::string&;
    static auto bind_environment(const RunnerContextAcquisitionSession&,
                                 std::string_view) noexcept -> std::expected<void, core::Error>;
    static auto register_requirements(
        const RunnerContextAcquisitionSession&,
        detail_HostSelectedResourceRequirements) noexcept -> std::expected<void, core::Error>;
    static auto canonical_projection(const MonitoringHostQualificationPlan&)
        -> std::expected<std::string, core::Error>;
    static auto requirements(const RunnerContextAcquisitionSession&) noexcept
        -> std::expected<std::reference_wrapper<const detail_HostSelectedResourceRequirements>,
                         core::Error>;
    static auto policy_identity(const MonitoringHostQualificationPlan&) noexcept
        -> const std::string&;
    static auto plan_binding(const MonitoringHostQualificationPlan&) noexcept
        -> const std::shared_ptr<const detail_HostSessionBinding>&;
};

} // namespace epycbench::monitoring::qualification
