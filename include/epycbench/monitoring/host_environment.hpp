#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/core/id.hpp"
#include "epycbench/core/quantity.hpp"
#include "epycbench/monitoring/host_runner_context_session.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace epycbench::monitoring::qualification {

class detail_HostSessionBinding;
class detail_HostQualificationPlanAccess;

struct HostEnvironmentEligibleNode {
    topology::StableNumaNodeId stable_numa_node_id;
    core::ByteCount installed_memory;
};

struct HostEnvironmentWorker {
    std::uint32_t worker_id{};
    topology::StableNumaNodeId stable_numa_node_id;
    topology::StableCoreId stable_core_id;
    topology::StableProcessingUnitId stable_pu_id;
    core::LinuxCpuId linux_cpu;
};

struct HostEnvironmentServiceCore {
    std::string role;
    topology::StableCoreId stable_core_id;
    std::vector<topology::StableProcessingUnitId> stable_pu_ids;
    std::vector<core::LinuxCpuId> linux_cpus;
    topology::StableProcessingUnitId execution_stable_pu_id;
    core::LinuxCpuId execution_linux_cpu;
};

class HostEnvironmentAuthority {
  public:
    HostEnvironmentAuthority(const HostEnvironmentAuthority&) = delete;
    HostEnvironmentAuthority(HostEnvironmentAuthority&&) noexcept = default;
    auto operator=(const HostEnvironmentAuthority&) -> HostEnvironmentAuthority& = delete;
    auto operator=(HostEnvironmentAuthority&&) -> HostEnvironmentAuthority& = delete;
    ~HostEnvironmentAuthority() noexcept = default;

    [[nodiscard]] auto eligible_nodes() const noexcept
        -> std::span<const HostEnvironmentEligibleNode> {
        return eligible_nodes_;
    }
    [[nodiscard]] auto bandwidth_workers() const noexcept
        -> std::span<const HostEnvironmentWorker> {
        return bandwidth_workers_;
    }
    [[nodiscard]] auto latency_workers() const noexcept -> std::span<const HostEnvironmentWorker> {
        return latency_workers_;
    }
    [[nodiscard]] auto service_cores() const noexcept
        -> std::span<const HostEnvironmentServiceCore> {
        return service_cores_;
    }
    [[nodiscard]] auto base_page_size() const noexcept -> core::ByteCount {
        return base_page_size_;
    }
    [[nodiscard]] auto canonical_json() const noexcept -> const std::string& {
        return canonical_json_;
    }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto environment_digest() const -> std::string;
    [[nodiscard]] auto physical_fingerprint() const noexcept
        -> const topology::PhysicalTopologyFingerprint& { return physical_; }
    [[nodiscard]] auto effective_fingerprint() const noexcept
        -> const topology::EffectiveTopologyFingerprint& { return effective_; }

  private:
    HostEnvironmentAuthority(std::vector<HostEnvironmentEligibleNode> eligible_nodes,
                             std::vector<HostEnvironmentWorker> bandwidth_workers,
                             std::vector<HostEnvironmentWorker> latency_workers,
                             std::vector<HostEnvironmentServiceCore> service_cores,
                             core::ByteCount base_page_size,
                             topology::PhysicalTopologyFingerprint physical,
                             topology::EffectiveTopologyFingerprint effective,
                             std::shared_ptr<const detail_HostSessionBinding> binding,
                             std::string canonical_json,
                             core::Sha256Digest digest)
        : eligible_nodes_{std::move(eligible_nodes)},
          bandwidth_workers_{std::move(bandwidth_workers)},
          latency_workers_{std::move(latency_workers)}, service_cores_{std::move(service_cores)},
          base_page_size_{base_page_size}, physical_{std::move(physical)},
          effective_{std::move(effective)}, binding_{std::move(binding)},
          canonical_json_{std::move(canonical_json)},
          digest_{digest} {}

    std::vector<HostEnvironmentEligibleNode> eligible_nodes_;
    std::vector<HostEnvironmentWorker> bandwidth_workers_;
    std::vector<HostEnvironmentWorker> latency_workers_;
    std::vector<HostEnvironmentServiceCore> service_cores_;
    core::ByteCount base_page_size_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    std::shared_ptr<const detail_HostSessionBinding> binding_;
    std::string canonical_json_;
    core::Sha256Digest digest_;
    friend class detail_HostEnvironmentFactory;
    friend class detail_HostQualificationPlanAccess;
};

[[nodiscard]] auto derive_host_environment(const RunnerContextAcquisitionSession& session) noexcept
    -> std::expected<HostEnvironmentAuthority, core::Error>;

} // namespace epycbench::monitoring::qualification
