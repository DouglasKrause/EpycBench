#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/runtime/ipc/payloads.hpp"
#include "epycbench/runtime/runner_context.hpp"

#include <expected>
#include <span>
#include <utility>
#include <vector>

namespace epycbench::runtime {

namespace detail {
struct AdmissionFactory;
struct PairwiseV2ContextualAdmissionAccess;
struct PairwiseV2AdmissionClassificationAccess;
}

enum class AdmittedPlacementIdentityVersion { v1, v2 };
class AdmittedPlacementIdentity {
  public:
    [[nodiscard]] auto version() const noexcept { return version_; }
    [[nodiscard]] auto digest() const noexcept -> const core::Sha256Digest& { return digest_; }
    [[nodiscard]] auto to_string() const -> std::string { return "sha256:" + digest_.to_hex(); }

    auto operator<=>(const AdmittedPlacementIdentity&) const = default;

  private:
    AdmittedPlacementIdentity(AdmittedPlacementIdentityVersion version, core::Sha256Digest digest)
      : version_{version}, digest_{digest} {}

    AdmittedPlacementIdentityVersion version_;
    core::Sha256Digest digest_;

    friend struct detail::AdmissionFactory;
    friend struct detail::PairwiseV2ContextualAdmissionAccess;
    friend struct detail::PairwiseV2AdmissionClassificationAccess;
};

using AdmittedBandwidthExecutionInputs = ipc::PlacementBandwidthExecutionInputs;
using AdmittedLatencyExecutionInputs = ipc::PlacementLatencyExecutionInputs;
using AdmittedKernelExecutionInputs = ipc::PlacementKernelExecutionInputs;
using AdmittedMeasurementInputs = ipc::PlacementMeasurementInputs;

class AdmittedExecutionInputs {
  public:
    [[nodiscard]] auto kernel() const noexcept -> const AdmittedKernelExecutionInputs& {
        return kernel_;
    }
    [[nodiscard]] auto measurement() const noexcept -> const AdmittedMeasurementInputs& {
        return measurement_;
    }

  private:
    explicit AdmittedExecutionInputs(const ipc::PlacementExecutionInputs& inputs)
        : kernel_{inputs.kernel()}, measurement_{inputs.measurement()} {}

    AdmittedKernelExecutionInputs kernel_;
    AdmittedMeasurementInputs measurement_;

    friend struct detail::AdmissionFactory;
};

class AdmittedWorker {
  public:
    [[nodiscard]] auto request() const noexcept -> const ipc::PlacementWorkerRequest& {
        return *request_;
    }
    [[nodiscard]] auto processing_unit() const noexcept -> core::ProcessingUnitId { return pu_; }
    [[nodiscard]] auto core() const noexcept -> core::CoreId { return core_; }
    [[nodiscard]] auto numa_nodes() const noexcept -> std::span<const core::NumaNodeId> {
        return numa_nodes_;
    }

  private:
    AdmittedWorker(const ipc::PlacementWorkerRequest& request, core::ProcessingUnitId pu,
                   core::CoreId core, std::vector<core::NumaNodeId> numa_nodes)
        : request_{&request}, pu_{pu}, core_{core}, numa_nodes_{std::move(numa_nodes)} {}

    const ipc::PlacementWorkerRequest* request_;
    core::ProcessingUnitId pu_;
    core::CoreId core_;
    std::vector<core::NumaNodeId> numa_nodes_;

    friend struct detail::AdmissionFactory;
};

class AdmittedServiceReservation {
  public:
    [[nodiscard]] auto cores() const noexcept -> std::span<const core::CoreId> { return cores_; }
    [[nodiscard]] auto effective_processing_units() const noexcept
        -> std::span<const core::ProcessingUnitId> {
        return effective_pus_;
    }
    [[nodiscard]] auto linux_cpus() const noexcept -> std::span<const core::LinuxCpuId> {
        return linux_cpus_;
    }

  private:
    AdmittedServiceReservation(std::vector<core::CoreId> cores,
                               std::vector<core::ProcessingUnitId> effective_pus,
                               std::vector<core::LinuxCpuId> linux_cpus)
        : cores_{std::move(cores)}, effective_pus_{std::move(effective_pus)},
          linux_cpus_{std::move(linux_cpus)} {}

    std::vector<core::CoreId> cores_;
    std::vector<core::ProcessingUnitId> effective_pus_;
    std::vector<core::LinuxCpuId> linux_cpus_;

    friend struct detail::AdmissionFactory;
};

class AdmittedPlacementRequest {
  public:
    [[nodiscard]] auto request_id() const noexcept -> ipc::RequestId { return request_id_; }
    [[nodiscard]] auto request() const noexcept -> const ipc::PlacementRequest& {
        return *request_;
    }
    [[nodiscard]] auto context() const noexcept -> const RunnerContext& { return *context_; }
    [[nodiscard]] auto workers() const noexcept -> std::span<const AdmittedWorker> {
        return workers_;
    }
    [[nodiscard]] auto service() const noexcept -> const AdmittedServiceReservation& {
        return service_;
    }
    [[nodiscard]] auto execution_inputs() const noexcept -> const AdmittedExecutionInputs& {
        return execution_inputs_;
    }
    [[nodiscard]] auto placement_identity() const noexcept
        -> const AdmittedPlacementIdentity& {
        return placement_identity_;
    }

  private:
    AdmittedPlacementRequest(ipc::RequestId request_id, const ipc::PlacementRequest& request,
                             const RunnerContext& context, std::vector<AdmittedWorker> workers,
                             AdmittedServiceReservation service,
                             AdmittedExecutionInputs execution_inputs,
                             AdmittedPlacementIdentity placement_identity)
        : request_id_{request_id}, request_{&request}, context_{&context},
          workers_{std::move(workers)}, service_{std::move(service)},
          execution_inputs_{std::move(execution_inputs)},
          placement_identity_{std::move(placement_identity)} {}

    ipc::RequestId request_id_;
    const ipc::PlacementRequest* request_;
    const RunnerContext* context_;
    std::vector<AdmittedWorker> workers_;
    AdmittedServiceReservation service_;
    AdmittedExecutionInputs execution_inputs_;
    AdmittedPlacementIdentity placement_identity_;

    friend struct detail::AdmissionFactory;
};

class AdmittedPairwiseProductionRequest {
  public:
    [[nodiscard]] static auto create(AdmittedPlacementRequest first,
                                     AdmittedPlacementRequest second,
                                     std::uint64_t resolved_comparison_order_seed) noexcept
        -> std::expected<AdmittedPairwiseProductionRequest, core::Error>;

    [[nodiscard]] auto first() const noexcept -> const AdmittedPlacementRequest& { return first_; }
    [[nodiscard]] auto second() const noexcept -> const AdmittedPlacementRequest& {
        return second_;
    }
    [[nodiscard]] auto resolved_comparison_order_seed() const noexcept -> std::uint64_t {
        return resolved_comparison_order_seed_;
    }

  private:
    AdmittedPairwiseProductionRequest(AdmittedPlacementRequest first,
                                      AdmittedPlacementRequest second,
                                      std::uint64_t resolved_comparison_order_seed)
        : first_{std::move(first)}, second_{std::move(second)},
          resolved_comparison_order_seed_{resolved_comparison_order_seed} {}

    AdmittedPlacementRequest first_;
    AdmittedPlacementRequest second_;
    std::uint64_t resolved_comparison_order_seed_;

    friend struct detail::PairwiseV2ContextualAdmissionAccess;
    friend struct detail::PairwiseV2AdmissionClassificationAccess;
};

[[nodiscard]] auto admit_placement_request(ipc::RequestId request_id,
                                           const ipc::PlacementRequest& request,
                                           const RunnerContext& context) noexcept
    -> std::expected<AdmittedPlacementRequest, core::Error>;

} // namespace epycbench::runtime
