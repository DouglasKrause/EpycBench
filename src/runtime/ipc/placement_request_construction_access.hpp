#pragma once

#include "core/json_ownership.hpp"
#include "epycbench/runtime/ipc/payloads.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace epycbench::runtime {
class RunnerContext;
auto project_placement_request(const scenario::ScenarioDigestedPlan&,
                               const scenario::MatrixCellId&) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;
auto project_placement_request(const scenario::ScenarioDigestedPlan&,
                               const scenario::MatrixCellId&, const RunnerContext&) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;
auto project_placement_request(const scenario::ScenarioDigestedPlan&,
                               const scenario::ComparisonVariantId&) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;
auto project_placement_request(const scenario::ScenarioDigestedPlan&,
                               const scenario::ComparisonVariantId&, const RunnerContext&) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error>;
}

namespace epycbench::runtime::ipc::detail {

struct RatioValues {
    std::uint64_t numerator{};
    std::uint64_t denominator{};
};

struct NumaValues {
    topology::StableNumaNodeId stable;
    core::LinuxNumaNodeId linux;
};
struct RegionValues {
    core::ByteCount bytes;
    PlacementPolicy policy;
    std::vector<NumaValues> nodes;
    PlacementPageMode page_mode;
    core::ByteCount page_size;
    RatioValues threshold;
    bool prefault{}, verify_before{}, verify_after{};
};
struct WorkerValues {
    topology::StableProcessingUnitId stable_pu;
    topology::StableCoreId stable_core;
    core::LinuxCpuId linux_cpu;
    RegionValues region;
};
struct MeasurementValues {
    core::Duration target, minimum, maximum;
    RatioValues overhead;
    bool calibration_enabled{};
    PlacementCalibrationStrategy calibration_strategy;
    std::uint64_t warmup_minimum{}, warmup_maximum{}, warmup_window{};
    RatioValues warmup_threshold;
    std::uint64_t repetition_minimum{}, repetition_maximum{}, minimum_valid{};
    bool stopping_enabled{};
    RatioValues stopping_confidence, stopping_precision;
    scenario::MatrixComparisonOrder comparison_order;
    std::uint64_t root_seed{};
};
struct BandwidthValues {
    kernel::SequentialReadBandwidthConfig configuration;
    std::optional<std::uint64_t> forbidden_seed;
    std::optional<scenario::PointerCycleSeedOrigin> forbidden_origin;
};
struct LatencyValues {
    kernel::DependentLoadLatencyConfig configuration;
    std::uint64_t seed{};
    scenario::PointerCycleSeedOrigin origin;
};
using KernelValues = std::variant<BandwidthValues, LatencyValues>;
struct MonitoringValues {
    std::vector<PlacementMonitoringChannel> required, optional;
    bool before{}, after{}, during{};
    std::optional<core::Duration> interval;
    PlacementOptionalUnavailableSourcePolicy unavailable;
};
struct PlacementRequestConstructionValues {
    PlacementRequestTransportVersion version;
    runtime::PlacementProvenance provenance;
    topology::PhysicalTopologyFingerprint physical;
    topology::EffectiveTopologyFingerprint effective;
    std::uint64_t experiment_index{};
    scenario::ScenarioIdentifier experiment_id;
    scenario::ScenarioIdentifier workload_id;
    kernel::KernelId kernel_id;
    KernelValues kernel;
    MeasurementValues measurement;
    std::optional<MonitoringValues> monitoring;
    std::vector<core::LinuxCpuId> service_cpus;
    std::vector<topology::StableCoreId> service_cores;
    std::vector<WorkerValues> workers;
    PlacementRequestConstructionValues(const PlacementRequestConstructionValues&) = default;
    auto operator=(const PlacementRequestConstructionValues& other)
        -> PlacementRequestConstructionValues& {
        if (this != &other) {
            this->~PlacementRequestConstructionValues();
            std::construct_at(this, other);
        }
        return *this;
    }

    PlacementRequestConstructionValues(PlacementRequestTransportVersion version_value,
       runtime::PlacementProvenance provenance_value, topology::PhysicalTopologyFingerprint physical_value,
       topology::EffectiveTopologyFingerprint effective_value, std::uint64_t experiment_index_value,
       scenario::ScenarioIdentifier experiment_id_value, scenario::ScenarioIdentifier workload_id_value,
       kernel::KernelId kernel_id_value, KernelValues kernel_value, MeasurementValues measurement_value,
       std::optional<MonitoringValues> monitoring_value, std::vector<core::LinuxCpuId> service_cpus_value,
       std::vector<topology::StableCoreId> service_cores_value, std::vector<WorkerValues> workers_value)
      : version{version_value}, provenance{std::move(provenance_value)}, physical{std::move(physical_value)},
        effective{std::move(effective_value)}, experiment_index{experiment_index_value},
        experiment_id{std::move(experiment_id_value)}, workload_id{std::move(workload_id_value)},
        kernel_id{kernel_id_value}, kernel{std::move(kernel_value)}, measurement{std::move(measurement_value)},
        monitoring{std::move(monitoring_value)}, service_cpus{std::move(service_cpus_value)},
        service_cores{std::move(service_cores_value)}, workers{std::move(workers_value)} {}
    PlacementRequestConstructionValues(scenario::ScenarioDigest scenario_digest,
       scenario::ResolvedScenarioPlanDigest resolved_digest, topology::PhysicalTopologyFingerprint physical_value,
       topology::EffectiveTopologyFingerprint effective_value, std::uint64_t experiment_index_value,
       scenario::ScenarioIdentifier experiment_id_value, scenario::ScenarioIdentifier workload_id_value,
       kernel::KernelId kernel_id_value, KernelValues kernel_value, MeasurementValues measurement_value,
       std::optional<MonitoringValues> monitoring_value, std::vector<core::LinuxCpuId> service_cpus_value,
       std::vector<topology::StableCoreId> service_cores_value, std::vector<WorkerValues> workers_value);
};

[[nodiscard]] auto validate_placement_request_local(const PlacementRequestConstructionValues&)
    -> std::expected<void, core::Error>;
[[nodiscard]] auto placement_request_wire(const PlacementRequestConstructionValues&)
    -> nlohmann::ordered_json;

struct PlacementRequestConstructionAccess {
  private:
    PlacementRequestConstructionAccess() = delete;
    [[nodiscard]] static auto construct(PlacementRequestConstructionValues)
        -> std::expected<PlacementRequest, core::Error>;
    static void retire_wire(PlacementRequest& request) noexcept;
    friend struct PlacementRequestParser;
    friend struct HostPlacementRequestConstructionAccess;
    friend auto
    epycbench::runtime::project_placement_request(const scenario::ScenarioDigestedPlan&,
                                                  const scenario::MatrixCellId&) noexcept
        -> std::expected<ipc::PlacementRequest, core::Error>;
    friend auto
    epycbench::runtime::project_placement_request(const scenario::ScenarioDigestedPlan&,
                                                  const scenario::MatrixCellId&,
                                                  const RunnerContext&) noexcept
        -> std::expected<ipc::PlacementRequest, core::Error>;
    friend auto
    epycbench::runtime::project_placement_request(const scenario::ScenarioDigestedPlan&,
                                                  const scenario::ComparisonVariantId&) noexcept
        -> std::expected<ipc::PlacementRequest, core::Error>;
    friend auto
    epycbench::runtime::project_placement_request(const scenario::ScenarioDigestedPlan&,
                                                  const scenario::ComparisonVariantId&,
                                                  const RunnerContext&) noexcept
        -> std::expected<ipc::PlacementRequest, core::Error>;
};

struct HostPlacementRequestConstructionAccess final {
    static void retire_wire(PlacementRequest& request) noexcept {
        PlacementRequestConstructionAccess::retire_wire(request);
    }
    [[nodiscard]] static auto parse_fallible(const nlohmann::json&,
                                             std::optional<core::Error>* = nullptr)
        -> std::expected<PlacementRequest, core::Error>;
    [[nodiscard]] static auto construct(PlacementRequestConstructionValues values)
        -> std::expected<PlacementRequest, core::Error> {
        return PlacementRequestConstructionAccess::construct(std::move(values));
    }
};

struct HostPlacementRequestCleanup final {
    PlacementRequest& request;
    ~HostPlacementRequestCleanup() noexcept {
        HostPlacementRequestConstructionAccess::retire_wire(request);
    }
};
struct HostPlacementRequestResultCleanup final {
    std::expected<PlacementRequest, core::Error>& result;
    ~HostPlacementRequestResultCleanup() noexcept {
        if (result)
            HostPlacementRequestConstructionAccess::retire_wire(*result);
        else
            core::detail::retire_json(result.error().context);
    }
};

} // namespace epycbench::runtime::ipc::detail
