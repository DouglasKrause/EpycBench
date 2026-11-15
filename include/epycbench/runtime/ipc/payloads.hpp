#pragma once

#include "epycbench/core/quantity.hpp"
#include "epycbench/kernel/configuration.hpp"
#include "epycbench/kernel/kernel_id.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"
#include "epycbench/runtime/placement_provenance.hpp"
#include "epycbench/scenario/exact_ratio.hpp"
#include "epycbench/scenario/identifier.hpp"
#include "epycbench/scenario/kernel_dispatch.hpp"
#include "epycbench/scenario/matrix_expansion.hpp"
#include "epycbench/scenario/scenario_plan_digest.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <cstdint>
#include <optional>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace epycbench::runtime::ipc {

namespace detail {
struct PlacementRequestParser;
struct PlacementRequestConstructionAccess;
struct HostPlacementRequestConstructionAccess;
struct CancelRequestParser;
} // namespace detail

enum class PlacementPolicy { bind };
enum class PlacementPageMode { base };

class PlacementRatio {
  public:
    [[nodiscard]] auto numerator() const noexcept -> std::uint64_t { return numerator_; }
    [[nodiscard]] auto denominator() const noexcept -> std::uint64_t { return denominator_; }
    auto operator==(const PlacementRatio&) const -> bool = default;

  private:
    PlacementRatio(std::uint64_t numerator, std::uint64_t denominator) noexcept
        : numerator_{numerator}, denominator_{denominator} {}

    std::uint64_t numerator_;
    std::uint64_t denominator_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementTopologyRequest {
  public:
    [[nodiscard]] auto physical_fingerprint() const noexcept
        -> const topology::PhysicalTopologyFingerprint& {
        return physical_;
    }
    [[nodiscard]] auto effective_fingerprint() const noexcept
        -> const topology::EffectiveTopologyFingerprint& {
        return effective_;
    }
    auto operator==(const PlacementTopologyRequest&) const -> bool = default;

  private:
    PlacementTopologyRequest(topology::PhysicalTopologyFingerprint physical,
                             topology::EffectiveTopologyFingerprint effective)
        : physical_{std::move(physical)}, effective_{std::move(effective)} {}

    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementExperimentRequest {
  public:
    [[nodiscard]] auto index() const noexcept -> std::uint64_t { return index_; }
    [[nodiscard]] auto id() const noexcept -> const scenario::ScenarioIdentifier& { return id_; }
    [[nodiscard]] auto workload_id() const noexcept -> const scenario::ScenarioIdentifier& {
        return workload_id_;
    }
    [[nodiscard]] auto kernel_id() const noexcept -> kernel::KernelId { return kernel_id_; }
    auto operator==(const PlacementExperimentRequest&) const -> bool = default;

  private:
    PlacementExperimentRequest(std::uint64_t index, scenario::ScenarioIdentifier id,
                               scenario::ScenarioIdentifier workload_id, kernel::KernelId kernel_id)
        : index_{index}, id_{std::move(id)}, workload_id_{std::move(workload_id)},
          kernel_id_{kernel_id} {}

    std::uint64_t index_;
    scenario::ScenarioIdentifier id_;
    scenario::ScenarioIdentifier workload_id_;
    kernel::KernelId kernel_id_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

enum class PlacementCalibrationStrategy { geometric_refine };

enum class PlacementMonitoringChannel { page_faults, context_switches, temperature, frequency };
enum class PlacementOptionalUnavailableSourcePolicy { fail, warn, disable };

class PlacementMonitoringInputs {
  public:
    [[nodiscard]] auto required() const noexcept -> std::span<const PlacementMonitoringChannel> {
        return required_;
    }
    [[nodiscard]] auto optional() const noexcept -> std::span<const PlacementMonitoringChannel> {
        return optional_;
    }
    [[nodiscard]] auto before_observation() const noexcept -> bool { return before_; }
    [[nodiscard]] auto after_observation() const noexcept -> bool { return after_; }
    [[nodiscard]] auto during_observation() const noexcept -> bool { return during_; }
    [[nodiscard]] auto during_interval() const noexcept -> const std::optional<core::Duration>& {
        return interval_;
    }
    [[nodiscard]] auto unavailable_optional_source() const noexcept
        -> PlacementOptionalUnavailableSourcePolicy {
        return unavailable_;
    }
    auto operator==(const PlacementMonitoringInputs&) const -> bool = default;

  private:
    PlacementMonitoringInputs(std::vector<PlacementMonitoringChannel> required,
                              std::vector<PlacementMonitoringChannel> optional, bool before,
                              bool after, bool during, std::optional<core::Duration> interval,
                              PlacementOptionalUnavailableSourcePolicy unavailable)
        : required_{std::move(required)}, optional_{std::move(optional)}, before_{before},
          after_{after}, during_{during}, interval_{interval}, unavailable_{unavailable} {}
    std::vector<PlacementMonitoringChannel> required_;
    std::vector<PlacementMonitoringChannel> optional_;
    bool before_;
    bool after_;
    bool during_;
    std::optional<core::Duration> interval_;
    PlacementOptionalUnavailableSourcePolicy unavailable_;
    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementMeasurementInputs {
  public:
    [[nodiscard]] auto observation_target_duration() const noexcept -> core::Duration {
        return observation_target_duration_;
    }
    [[nodiscard]] auto observation_minimum_duration() const noexcept -> core::Duration {
        return observation_minimum_duration_;
    }
    [[nodiscard]] auto observation_maximum_duration() const noexcept -> core::Duration {
        return observation_maximum_duration_;
    }
    [[nodiscard]] auto maximum_harness_overhead() const noexcept -> const scenario::ExactRatio& {
        return maximum_harness_overhead_;
    }
    [[nodiscard]] auto calibration_enabled() const noexcept -> bool { return calibration_enabled_; }
    [[nodiscard]] auto calibration_strategy() const noexcept -> PlacementCalibrationStrategy {
        return calibration_strategy_;
    }
    [[nodiscard]] auto warmup_minimum() const noexcept -> std::uint64_t { return warmup_minimum_; }
    [[nodiscard]] auto warmup_maximum() const noexcept -> std::uint64_t { return warmup_maximum_; }
    [[nodiscard]] auto warmup_stability_window() const noexcept -> std::uint64_t {
        return warmup_stability_window_;
    }
    [[nodiscard]] auto warmup_stability_threshold() const noexcept -> const scenario::ExactRatio& {
        return warmup_stability_threshold_;
    }
    [[nodiscard]] auto repetition_minimum_blocks() const noexcept -> std::uint64_t {
        return repetition_minimum_blocks_;
    }
    [[nodiscard]] auto repetition_maximum_blocks() const noexcept -> std::uint64_t {
        return repetition_maximum_blocks_;
    }
    [[nodiscard]] auto minimum_valid_observations_per_variant() const noexcept -> std::uint64_t {
        return minimum_valid_observations_per_variant_;
    }
    [[nodiscard]] auto stopping_enabled() const noexcept -> bool { return stopping_enabled_; }
    [[nodiscard]] auto stopping_confidence() const noexcept -> const scenario::ExactRatio& {
        return stopping_confidence_;
    }
    [[nodiscard]] auto stopping_precision() const noexcept -> const scenario::ExactRatio& {
        return stopping_precision_;
    }
    [[nodiscard]] auto comparison_order() const noexcept -> scenario::MatrixComparisonOrder {
        return comparison_order_;
    }
    [[nodiscard]] auto root_seed() const noexcept -> std::uint64_t { return root_seed_; }
    auto operator==(const PlacementMeasurementInputs&) const -> bool = default;

  private:
    PlacementMeasurementInputs(core::Duration target, core::Duration minimum,
                               core::Duration maximum, scenario::ExactRatio overhead,
                               bool calibration_enabled,
                               PlacementCalibrationStrategy calibration_strategy,
                               std::uint64_t warmup_minimum, std::uint64_t warmup_maximum,
                               std::uint64_t warmup_window, scenario::ExactRatio warmup_threshold,
                               std::uint64_t repetition_minimum, std::uint64_t repetition_maximum,
                               std::uint64_t minimum_valid_observations, bool stopping_enabled,
                               scenario::ExactRatio stopping_confidence,
                               scenario::ExactRatio stopping_precision,
                               scenario::MatrixComparisonOrder comparison_order,
                               std::uint64_t root_seed)
        : observation_target_duration_{target}, observation_minimum_duration_{minimum},
          observation_maximum_duration_{maximum}, maximum_harness_overhead_{overhead},
          calibration_enabled_{calibration_enabled}, calibration_strategy_{calibration_strategy},
          warmup_minimum_{warmup_minimum}, warmup_maximum_{warmup_maximum},
          warmup_stability_window_{warmup_window}, warmup_stability_threshold_{warmup_threshold},
          repetition_minimum_blocks_{repetition_minimum},
          repetition_maximum_blocks_{repetition_maximum},
          minimum_valid_observations_per_variant_{minimum_valid_observations},
          stopping_enabled_{stopping_enabled}, stopping_confidence_{stopping_confidence},
          stopping_precision_{stopping_precision}, comparison_order_{comparison_order},
          root_seed_{root_seed} {}

    core::Duration observation_target_duration_;
    core::Duration observation_minimum_duration_;
    core::Duration observation_maximum_duration_;
    scenario::ExactRatio maximum_harness_overhead_;
    bool calibration_enabled_;
    PlacementCalibrationStrategy calibration_strategy_;
    std::uint64_t warmup_minimum_;
    std::uint64_t warmup_maximum_;
    std::uint64_t warmup_stability_window_;
    scenario::ExactRatio warmup_stability_threshold_;
    std::uint64_t repetition_minimum_blocks_;
    std::uint64_t repetition_maximum_blocks_;
    std::uint64_t minimum_valid_observations_per_variant_;
    bool stopping_enabled_;
    scenario::ExactRatio stopping_confidence_;
    scenario::ExactRatio stopping_precision_;
    scenario::MatrixComparisonOrder comparison_order_;
    std::uint64_t root_seed_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementBandwidthExecutionInputs {
  public:
    [[nodiscard]] auto configuration() const noexcept
        -> const kernel::SequentialReadBandwidthConfig& {
        return configuration_;
    }
    auto operator==(const PlacementBandwidthExecutionInputs&) const -> bool = default;

  private:
    explicit PlacementBandwidthExecutionInputs(kernel::SequentialReadBandwidthConfig configuration)
        : configuration_{std::move(configuration)} {}
    kernel::SequentialReadBandwidthConfig configuration_;
    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementLatencyExecutionInputs {
  public:
    [[nodiscard]] auto configuration() const noexcept -> const kernel::DependentLoadLatencyConfig& {
        return configuration_;
    }
    [[nodiscard]] auto resolved_permutation_seed() const noexcept -> std::uint64_t { return seed_; }
    [[nodiscard]] auto seed_origin() const noexcept -> scenario::PointerCycleSeedOrigin {
        return origin_;
    }
    auto operator==(const PlacementLatencyExecutionInputs&) const -> bool = default;

  private:
    PlacementLatencyExecutionInputs(kernel::DependentLoadLatencyConfig configuration,
                                    std::uint64_t seed, scenario::PointerCycleSeedOrigin origin)
        : configuration_{std::move(configuration)}, seed_{seed}, origin_{origin} {}
    kernel::DependentLoadLatencyConfig configuration_;
    std::uint64_t seed_;
    scenario::PointerCycleSeedOrigin origin_;
    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

using PlacementKernelExecutionInputs =
    std::variant<PlacementBandwidthExecutionInputs, PlacementLatencyExecutionInputs>;

class PlacementExecutionInputs {
  public:
    [[nodiscard]] auto kernel() const noexcept -> const PlacementKernelExecutionInputs& {
        return kernel_;
    }
    [[nodiscard]] auto measurement() const noexcept -> const PlacementMeasurementInputs& {
        return measurement_;
    }
    auto operator==(const PlacementExecutionInputs&) const -> bool = default;

  private:
    PlacementExecutionInputs(PlacementKernelExecutionInputs kernel,
                             PlacementMeasurementInputs measurement)
        : kernel_{std::move(kernel)}, measurement_{std::move(measurement)} {}
    PlacementKernelExecutionInputs kernel_;
    PlacementMeasurementInputs measurement_;
    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementServiceReservation {
  public:
    [[nodiscard]] auto linux_cpus() const noexcept -> std::span<const core::LinuxCpuId> {
        return linux_cpus_;
    }
    [[nodiscard]] auto stable_cores() const noexcept -> std::span<const topology::StableCoreId> {
        return stable_cores_;
    }
    auto operator==(const PlacementServiceReservation&) const -> bool = default;

  private:
    PlacementServiceReservation(std::vector<core::LinuxCpuId> linux_cpus,
                                std::vector<topology::StableCoreId> stable_cores)
        : linux_cpus_{std::move(linux_cpus)}, stable_cores_{std::move(stable_cores)} {}

    std::vector<core::LinuxCpuId> linux_cpus_;
    std::vector<topology::StableCoreId> stable_cores_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementNumaTarget {
  public:
    [[nodiscard]] auto stable_numa_node_id() const noexcept -> const topology::StableNumaNodeId& {
        return stable_numa_node_id_;
    }
    [[nodiscard]] auto linux_numa_node() const noexcept -> core::LinuxNumaNodeId {
        return linux_numa_node_;
    }
    auto operator==(const PlacementNumaTarget&) const -> bool = default;

  private:
    PlacementNumaTarget(topology::StableNumaNodeId stable_numa_node_id,
                        core::LinuxNumaNodeId linux_numa_node)
        : stable_numa_node_id_{std::move(stable_numa_node_id)}, linux_numa_node_{linux_numa_node} {}

    topology::StableNumaNodeId stable_numa_node_id_;
    core::LinuxNumaNodeId linux_numa_node_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementRegionRequest {
  public:
    [[nodiscard]] auto bytes() const noexcept -> core::ByteCount { return bytes_; }
    [[nodiscard]] auto policy() const noexcept -> PlacementPolicy { return policy_; }
    [[nodiscard]] auto nodes() const noexcept -> std::span<const PlacementNumaTarget> {
        return nodes_;
    }
    [[nodiscard]] auto page_mode() const noexcept -> PlacementPageMode { return page_mode_; }
    [[nodiscard]] auto page_size_bytes() const noexcept -> core::ByteCount {
        return page_size_bytes_;
    }
    [[nodiscard]] auto placement_threshold() const noexcept -> const PlacementRatio& {
        return placement_threshold_;
    }
    [[nodiscard]] auto prefault() const noexcept -> bool { return prefault_; }
    [[nodiscard]] auto verify_before() const noexcept -> bool { return verify_before_; }
    [[nodiscard]] auto verify_after() const noexcept -> bool { return verify_after_; }
    auto operator==(const PlacementRegionRequest&) const -> bool = default;

  private:
    PlacementRegionRequest(core::ByteCount bytes, PlacementPolicy policy,
                           std::vector<PlacementNumaTarget> nodes, PlacementPageMode page_mode,
                           core::ByteCount page_size_bytes, PlacementRatio placement_threshold,
                           bool prefault, bool verify_before, bool verify_after)
        : bytes_{bytes}, policy_{policy}, nodes_{std::move(nodes)}, page_mode_{page_mode},
          page_size_bytes_{page_size_bytes}, placement_threshold_{placement_threshold},
          prefault_{prefault}, verify_before_{verify_before}, verify_after_{verify_after} {}

    core::ByteCount bytes_;
    PlacementPolicy policy_;
    std::vector<PlacementNumaTarget> nodes_;
    PlacementPageMode page_mode_;
    core::ByteCount page_size_bytes_;
    PlacementRatio placement_threshold_;
    bool prefault_;
    bool verify_before_;
    bool verify_after_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementCpuAssignment {
  public:
    [[nodiscard]] auto stable_pu_id() const noexcept -> const topology::StableProcessingUnitId& {
        return stable_pu_id_;
    }
    [[nodiscard]] auto stable_core_id() const noexcept -> const topology::StableCoreId& {
        return stable_core_id_;
    }
    [[nodiscard]] auto linux_cpu() const noexcept -> core::LinuxCpuId { return linux_cpu_; }
    auto operator==(const PlacementCpuAssignment&) const -> bool = default;

  private:
    PlacementCpuAssignment(topology::StableProcessingUnitId stable_pu_id,
                           topology::StableCoreId stable_core_id, core::LinuxCpuId linux_cpu)
        : stable_pu_id_{std::move(stable_pu_id)}, stable_core_id_{std::move(stable_core_id)},
          linux_cpu_{linux_cpu} {}

    topology::StableProcessingUnitId stable_pu_id_;
    topology::StableCoreId stable_core_id_;
    core::LinuxCpuId linux_cpu_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

class PlacementWorkerRequest {
  public:
    [[nodiscard]] auto cpu() const noexcept -> const PlacementCpuAssignment& { return cpu_; }
    [[nodiscard]] auto region() const noexcept -> const PlacementRegionRequest& { return region_; }
    auto operator==(const PlacementWorkerRequest&) const -> bool = default;

  private:
    PlacementWorkerRequest(PlacementCpuAssignment cpu, PlacementRegionRequest region)
        : cpu_{std::move(cpu)}, region_{std::move(region)} {}

    PlacementCpuAssignment cpu_;
    PlacementRegionRequest region_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

enum class PlacementRequestTransportVersion { v2, v3, v4 };
enum class PlacementProofTransportVersion { v1, v2 };

class PlacementRequest {
  public:
    PlacementRequest(const PlacementRequest&) = default;
    PlacementRequest(PlacementRequest&&) noexcept = default;
    auto operator=(const PlacementRequest& other) -> PlacementRequest& {
        if(this!=&other){this->~PlacementRequest();std::construct_at(this,other);} return *this;
    }
    auto operator=(PlacementRequest&& other) noexcept -> PlacementRequest& {
        if(this!=&other){this->~PlacementRequest();std::construct_at(this,std::move(other));} return *this;
    }
    [[nodiscard]] auto transport_version() const noexcept { return version_; }
    [[nodiscard]] auto provenance() const noexcept -> const runtime::PlacementProvenance& { return provenance_; }
    [[nodiscard]] auto scenario_provenance() const noexcept
        -> std::expected<std::reference_wrapper<const runtime::ScenarioPlacementProvenance>, core::Error> {
        return runtime::scenario_provenance(provenance_);
    }
    [[nodiscard]] auto host_qualification_plan_provenance() const noexcept
        -> std::expected<std::reference_wrapper<const runtime::HostQualificationPlanPlacementProvenance>, core::Error> {
        return runtime::host_qualification_plan_provenance(provenance_);
    }
    [[nodiscard]] auto topology() const noexcept -> const PlacementTopologyRequest& {
        return topology_;
    }
    [[nodiscard]] auto experiment() const noexcept -> const PlacementExperimentRequest& {
        return experiment_;
    }
    [[nodiscard]] auto execution_inputs() const noexcept -> const PlacementExecutionInputs& {
        return execution_inputs_;
    }
    [[nodiscard]] auto monitoring_inputs() const noexcept
        -> const std::optional<PlacementMonitoringInputs>& {
        return monitoring_inputs_;
    }
    [[nodiscard]] auto reserved_service() const noexcept -> const PlacementServiceReservation& {
        return reserved_service_;
    }
    [[nodiscard]] auto workers() const noexcept -> std::span<const PlacementWorkerRequest> {
        return workers_;
    }
    [[nodiscard]] auto wire() const noexcept -> const nlohmann::ordered_json& { return wire_; }
    auto operator==(const PlacementRequest&) const -> bool = default;

  private:
    PlacementRequest(PlacementRequestTransportVersion version,
                     runtime::PlacementProvenance provenance,
                     PlacementTopologyRequest topology, PlacementExperimentRequest experiment,
                     PlacementExecutionInputs execution_inputs,
                     std::optional<PlacementMonitoringInputs> monitoring_inputs,
                     PlacementServiceReservation reserved_service,
                     std::vector<PlacementWorkerRequest> workers, nlohmann::ordered_json wire)
        : version_{version}, provenance_{std::move(provenance)}, topology_{std::move(topology)},
          experiment_{std::move(experiment)}, execution_inputs_{std::move(execution_inputs)},
          monitoring_inputs_{std::move(monitoring_inputs)},
          reserved_service_{std::move(reserved_service)}, workers_{std::move(workers)},
          wire_(std::move(wire)) {}

    PlacementRequestTransportVersion version_;
    runtime::PlacementProvenance provenance_;
    PlacementTopologyRequest topology_;
    PlacementExperimentRequest experiment_;
    PlacementExecutionInputs execution_inputs_;
    std::optional<PlacementMonitoringInputs> monitoring_inputs_;
    PlacementServiceReservation reserved_service_;
    std::vector<PlacementWorkerRequest> workers_;
    nlohmann::ordered_json wire_;

    friend struct detail::PlacementRequestParser;
    friend struct detail::PlacementRequestConstructionAccess;
};

struct PlacementProof {
    PlacementProofTransportVersion version;
    runtime::PlacementProvenance provenance;
    topology::PhysicalTopologyFingerprint physical;
    topology::EffectiveTopologyFingerprint effective;
    scenario::ScenarioIdentifier experiment_id;
    scenario::ScenarioIdentifier workload_id;
    kernel::KernelId kernel_id;
    nlohmann::ordered_json wire;
    auto operator==(const PlacementProof&) const -> bool = default;
};

enum class CancelReason { user_interrupt, controller_shutdown, controller_timeout };
enum class RunnerPhase { startup, placement, cleanup };

class CancelRequest {
  public:
    [[nodiscard]] auto reason() const noexcept -> CancelReason { return reason_; }

  private:
    explicit CancelRequest(CancelReason reason) noexcept : reason_{reason} {}

    CancelReason reason_;

    friend struct detail::CancelRequestParser;
};

[[nodiscard]] auto parse_placement_request(const nlohmann::json&)
    -> std::expected<PlacementRequest, core::Error>;
[[nodiscard]] auto parse_placement_request(const Envelope&)
    -> std::expected<PlacementRequest, core::Error>;
// This checked receiving-side parser requires a cancel_request envelope and reuses
// validate_payload as the authoritative payload grammar. Callers still validate and advance
// RunnerControlState before parsing when processing the control stream.
[[nodiscard]] auto parse_cancel_request(const Envelope&)
    -> std::expected<CancelRequest, core::Error>;
[[nodiscard]] auto parse_placement_proof(const nlohmann::json&)
    -> std::expected<PlacementProof, core::Error>;
[[nodiscard]] auto validate_placement_proof(const PlacementRequest&, const PlacementProof&)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto validate_payload(MessageType, const nlohmann::json&)
    -> std::expected<void, core::Error>;

class PlacementEventState {
  public:
    PlacementEventState(RequestId id, PlacementRequest request)
        : state_{id}, request_{std::move(request)} {}
    PlacementEventState(const PlacementEventState&) = default;
    auto operator=(PlacementEventState&& other) noexcept -> PlacementEventState& {
        state_ = std::move(other.state_);
        return *this;
    }
    [[nodiscard]] auto accept(const Envelope&) -> std::expected<void, core::Error>;
    [[nodiscard]] auto end() -> std::expected<void, core::Error> { return state_.end(); }

  private:
    ControllerEventState state_;
    PlacementRequest request_;
};

} // namespace epycbench::runtime::ipc
