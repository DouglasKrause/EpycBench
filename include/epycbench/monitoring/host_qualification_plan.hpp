#pragma once

#include "epycbench/monitoring/host_environment.hpp"

#include <array>
#include <memory>

namespace epycbench::monitoring::qualification {

class detail_HostSessionBinding;
class detail_HostQualificationCompositionAccess;

enum class HostPlanVariantId { first, second };
enum class HostPlanMemoryPolicy { bind };
enum class HostPlanMonitoringConfiguration { c0 };
enum class HostPlanServiceRole { control, measurement };

struct HostPlanWorkerMemoryBinding final {
    HostPlanVariantId variant;
    std::uint32_t worker_id;
    core::LinuxCpuId linux_cpu;
    core::LinuxNumaNodeId linux_node;
};

class MonitoringHostQualificationPlan final {
  public:
    MonitoringHostQualificationPlan(const MonitoringHostQualificationPlan&) = delete;
    MonitoringHostQualificationPlan(MonitoringHostQualificationPlan&&) noexcept = default;
    auto operator=(const MonitoringHostQualificationPlan&)
        -> MonitoringHostQualificationPlan& = delete;
    auto operator=(MonitoringHostQualificationPlan&&)
        -> MonitoringHostQualificationPlan& = delete;
    ~MonitoringHostQualificationPlan() noexcept = default;

    [[nodiscard]] auto bandwidth_descriptor() const noexcept -> const kernel::KernelDescriptor& {
        return bandwidth_descriptor_;
    }
    [[nodiscard]] auto latency_descriptor() const noexcept -> const kernel::KernelDescriptor& {
        return latency_descriptor_;
    }
    [[nodiscard]] auto calibration_policy() const noexcept
        -> const measurement::CalibrationDurationPolicy& { return calibration_policy_; }
    [[nodiscard]] auto bandwidth_workers() const noexcept
        -> std::span<const HostEnvironmentWorker> { return bandwidth_workers_; }
    [[nodiscard]] auto latency_workers() const noexcept
        -> std::span<const HostEnvironmentWorker> { return latency_workers_; }
    [[nodiscard]] auto service_cores() const noexcept
        -> std::span<const HostEnvironmentServiceCore> { return service_cores_; }
    [[nodiscard]] auto working_set_bytes() const noexcept -> core::ByteCount {
        return working_set_bytes_;
    }
    [[nodiscard]] auto base_page_size() const noexcept -> core::ByteCount { return base_page_size_; }
    [[nodiscard]] auto memory_policy() const noexcept -> HostPlanMemoryPolicy {
        return HostPlanMemoryPolicy::bind;
    }
    [[nodiscard]] auto worker_local_memory() const noexcept -> bool { return true; }
    [[nodiscard]] auto bandwidth_memory_bindings() const noexcept
        ->std::span<const HostPlanWorkerMemoryBinding> { return bandwidth_memory_bindings_; }
    [[nodiscard]] auto latency_memory_bindings() const noexcept
        ->std::span<const HostPlanWorkerMemoryBinding> { return latency_memory_bindings_; }
    [[nodiscard]] auto monitoring_configuration() const noexcept
        ->HostPlanMonitoringConfiguration { return HostPlanMonitoringConfiguration::c0; }
    [[nodiscard]] auto active_service_roles() const noexcept
        ->std::span<const HostPlanServiceRole> { return active_service_roles_; }
    [[nodiscard]] auto template_sha256() const noexcept -> const core::Sha256Digest& {
        return template_sha256_;
    }
    [[nodiscard]] auto environment_digest() const noexcept -> const std::string& {
        return environment_digest_;
    }
    [[nodiscard]] auto physical_fingerprint() const noexcept
        -> const topology::PhysicalTopologyFingerprint& { return physical_; }
    [[nodiscard]] auto effective_fingerprint() const noexcept
        -> const topology::EffectiveTopologyFingerprint& { return effective_; }
    [[nodiscard]] auto canonical_json() const noexcept -> const std::string& { return canonical_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto plan_digest() const -> std::string { return "sha256:"+digest_.to_hex(); }

  private:
    MonitoringHostQualificationPlan(
        kernel::KernelDescriptor bandwidth, kernel::KernelDescriptor latency,
        measurement::CalibrationDurationPolicy calibration,
        std::vector<HostEnvironmentWorker> bandwidth_workers,
        std::vector<HostEnvironmentWorker> latency_workers,
        std::vector<HostEnvironmentServiceCore> service,
        core::ByteCount working_set,core::ByteCount base_page_size,
        std::vector<HostPlanWorkerMemoryBinding> bandwidth_memory,
        std::vector<HostPlanWorkerMemoryBinding> latency_memory,
        std::string policy_identity, core::Sha256Digest template_sha256,
        std::string environment_digest,
        topology::PhysicalTopologyFingerprint physical,
        topology::EffectiveTopologyFingerprint effective,
        std::shared_ptr<const detail_HostSessionBinding> binding,
        std::string canonical, core::Sha256Digest digest)
        : bandwidth_descriptor_{std::move(bandwidth)}, latency_descriptor_{std::move(latency)},
          calibration_policy_{std::move(calibration)},
          bandwidth_workers_{std::move(bandwidth_workers)},
          latency_workers_{std::move(latency_workers)}, service_cores_{std::move(service)},
          working_set_bytes_{working_set},base_page_size_{base_page_size},
          bandwidth_memory_bindings_{std::move(bandwidth_memory)},
          latency_memory_bindings_{std::move(latency_memory)},
          policy_identity_{std::move(policy_identity)}, template_sha256_{template_sha256},
          environment_digest_{std::move(environment_digest)},
          physical_{std::move(physical)}, effective_{std::move(effective)},
          binding_{std::move(binding)},
          canonical_{std::move(canonical)}, digest_{digest} {}

    kernel::KernelDescriptor bandwidth_descriptor_;
    kernel::KernelDescriptor latency_descriptor_;
    measurement::CalibrationDurationPolicy calibration_policy_;
    std::vector<HostEnvironmentWorker> bandwidth_workers_;
    std::vector<HostEnvironmentWorker> latency_workers_;
    std::vector<HostEnvironmentServiceCore> service_cores_;
    core::ByteCount working_set_bytes_;
    core::ByteCount base_page_size_;
    std::vector<HostPlanWorkerMemoryBinding> bandwidth_memory_bindings_;
    std::vector<HostPlanWorkerMemoryBinding> latency_memory_bindings_;
    std::array<HostPlanServiceRole,1> active_service_roles_{HostPlanServiceRole::control};
    std::string policy_identity_;
    core::Sha256Digest template_sha256_;
    std::string environment_digest_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    std::shared_ptr<const detail_HostSessionBinding> binding_;
    std::string canonical_;
    core::Sha256Digest digest_;
    friend class detail_MonitoringHostQualificationPlanFactory;
    friend class detail_HostQualificationCompositionAccess;
    friend class detail_HostQualificationPlanAccess;
};

[[nodiscard]] auto resolve_monitoring_host_qualification_plan(
    const RunnerContextAcquisitionSession&, const HostEnvironmentAuthority&) noexcept
    -> std::expected<MonitoringHostQualificationPlan, core::Error>;

} // namespace epycbench::monitoring::qualification
