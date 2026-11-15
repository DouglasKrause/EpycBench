#pragma once

#include "epycbench/runtime/admitted_placement_request.hpp"

#include <expected>
#include <span>
#include <vector>

namespace epycbench::runtime {

namespace detail {
struct PairwiseV2ContextualAdmissionAccess;
struct PairwiseV2AdmissionClassificationAccess;
}

enum class ServiceRole { control_monitor, measurement_monitor };

struct ServiceThreadAssignment {
    ServiceRole role;
    topology::StableProcessingUnitId stable_pu;
    topology::StableCoreId stable_core;
    core::ProcessingUnitId processing_unit;
    core::CoreId core;
    core::LinuxCpuId linux_cpu;

    auto operator==(const ServiceThreadAssignment&) const -> bool = default;
};

class ServicePlacementPlan {
  public:
    [[nodiscard]] static auto create(const AdmittedPlacementRequest& request,
                                     bool measurement_monitor_required = false) noexcept
        -> std::expected<ServicePlacementPlan, core::Error>;

    [[nodiscard]] auto assignments() const noexcept -> std::span<const ServiceThreadAssignment>;
    [[nodiscard]] auto reserved_cores() const noexcept -> std::span<const core::CoreId>;
    [[nodiscard]] auto reserved_processing_units() const noexcept
        -> std::span<const core::ProcessingUnitId>;
    [[nodiscard]] auto reserved_linux_cpus() const noexcept -> std::span<const core::LinuxCpuId>;
    [[nodiscard]] auto unassigned_processing_units() const noexcept
        -> std::span<const core::ProcessingUnitId>;
    [[nodiscard]] auto unassigned_linux_cpus() const noexcept -> std::span<const core::LinuxCpuId>;

  private:
    ServicePlacementPlan(std::vector<ServiceThreadAssignment> assignments,
                         std::vector<core::CoreId> cores,
                         std::vector<core::ProcessingUnitId> processing_units,
                         std::vector<core::LinuxCpuId> linux_cpus,
                         std::vector<core::ProcessingUnitId> unassigned_processing_units,
                         std::vector<core::LinuxCpuId> unassigned_linux_cpus) noexcept;

    std::vector<ServiceThreadAssignment> assignments_;
    std::vector<core::CoreId> reserved_cores_;
    std::vector<core::ProcessingUnitId> reserved_processing_units_;
    std::vector<core::LinuxCpuId> reserved_linux_cpus_;
    std::vector<core::ProcessingUnitId> unassigned_processing_units_;
    std::vector<core::LinuxCpuId> unassigned_linux_cpus_;

    friend struct detail::PairwiseV2ContextualAdmissionAccess;
    friend struct detail::PairwiseV2AdmissionClassificationAccess;
};

} // namespace epycbench::runtime
