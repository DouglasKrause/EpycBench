#pragma once

#include "epycbench/kernel/dependent_load_latency.hpp"
#include "epycbench/kernel/sequential_read_bandwidth.hpp"
#include "epycbench/monitoring/host_q0b_v2_projection.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"
#include "monitoring/host_qualification_composition.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace epycbench::monitoring::qualification::detail {

struct HostFrozenObservationAdmissionTestAccess;

enum class HostFrozenConfiguration { c0, c1, c2, c3, c4 };
enum class HostFrozenAdmissionReason {
    invalid_selector, unavailable_input, worker_bound, work_bound,
    original_facts, profile_binding, request, request_id, placement,
    monitoring_policy, service_placement, allocation, contained_exception
};
// First failed predicate in the existing Q0b binding order; none means that
// this failure did not arise from that binding comparison.
enum class HostFrozenBindingCheckpoint : unsigned char {
    none, environment_schema, environment_digest, qualification_plan_schema,
    qualification_plan_digest, production_inputs_schema, production_inputs_sha256,
    base_environment_schema, base_environment_sha256, bandwidth_family, latency_family,
    policy_identity, policy_sha256
};
struct HostFrozenAdmissionFailure final {
    HostFrozenAdmissionReason reason;
    std::optional<core::Error> lower;
    HostFrozenBindingCheckpoint binding_checkpoint{HostFrozenBindingCheckpoint::none};
    HostFrozenAdmissionFailure(HostFrozenAdmissionReason, std::optional<core::Error>,
        HostFrozenBindingCheckpoint = HostFrozenBindingCheckpoint::none) noexcept;
    HostFrozenAdmissionFailure(HostFrozenAdmissionFailure&&) noexcept = default;
    HostFrozenAdmissionFailure(const HostFrozenAdmissionFailure&) = delete;
    ~HostFrozenAdmissionFailure() noexcept;
};
using HostFrozenWork = std::variant<kernel::SequentialReadBandwidthPassCount,
                                    kernel::DependentLoadLatencyStepCount>;

// Every reference belongs to stable owner stock, except the admitted context,
// which belongs to the original Session. Both must outlive any future consumer.
struct HostFrozenObservationView final {
    HostQ0bFamily family;
    HostFrozenConfiguration configuration;
    std::reference_wrapper<const runtime::ipc::PlacementRequest> request;
    std::reference_wrapper<const runtime::AdmittedPlacementRequest> placement;
    std::reference_wrapper<const MonitoringExecutionPolicy> policy;
    std::reference_wrapper<const runtime::ServicePlacementPlan> service;
    std::reference_wrapper<const MonitoringHostAdmittedProductionFacts> original_facts;
    std::reference_wrapper<const core::Sha256Digest> profile_sha256;
    std::reference_wrapper<const HostFrozenWork> work;
};

class HostFrozenObservationAdmission final {
  public:
    HostFrozenObservationAdmission(const HostFrozenObservationAdmission&) = delete;
    auto operator=(const HostFrozenObservationAdmission&)
        -> HostFrozenObservationAdmission& = delete;
    HostFrozenObservationAdmission(HostFrozenObservationAdmission&&) noexcept;
    auto operator=(HostFrozenObservationAdmission&&)
        -> HostFrozenObservationAdmission& = delete;
    ~HostFrozenObservationAdmission() noexcept;

    [[nodiscard]] static auto create(
        const RunnerContextAcquisitionSession&, const MonitoringHostQualificationPlan&,
        const HostQualificationCalibrationInputs&, const MonitoringQ1ValidatedPublicValues&,
        runtime::ProductionRequestIdSource&, HostQ0bFamily, HostFrozenConfiguration) noexcept
        -> std::expected<HostFrozenObservationAdmission, HostFrozenAdmissionFailure>;
    [[nodiscard]] static auto
    create_source(const RunnerContextAcquisitionSession&, const HostEnvironmentAuthority&,
                  const MonitoringHostQualificationPlan&, const HostQualificationCalibrationInputs&,
                  const MonitoringQ1ValidatedPublicValues&, runtime::ProductionRequestIdSource&,
                  HostQ0bFamily, HostFrozenConfiguration, std::optional<core::Error>* = nullptr)
        -> std::expected<HostFrozenObservationAdmission, HostFrozenAdmissionFailure>;
    [[nodiscard]] auto view() const noexcept -> std::optional<HostFrozenObservationView>;

  private:
    [[nodiscard]] static auto
    create_fallible(const RunnerContextAcquisitionSession&, const HostEnvironmentAuthority*,
                    const MonitoringHostQualificationPlan&,
                    const HostQualificationCalibrationInputs&,
                    const MonitoringQ1ValidatedPublicValues&, runtime::ProductionRequestIdSource&,
                    HostQ0bFamily, HostFrozenConfiguration, std::optional<core::Error>*)
        -> std::expected<HostFrozenObservationAdmission, HostFrozenAdmissionFailure>;
    [[nodiscard]] static auto monitoring_wire(HostFrozenConfiguration) -> nlohmann::json;
    struct State;
    explicit HostFrozenObservationAdmission(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend struct HostFrozenObservationAdmissionTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
