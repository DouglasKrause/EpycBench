#pragma once

#include "epycbench/runtime/calibration_only_production.hpp"
#include "epycbench/runtime/runner_context.hpp"
#include <optional>

namespace epycbench::runtime::detail {

struct CalibrationOnlyCompatibilityFacts {
    bool context_mismatch{};
    bool scenario_identity_mismatch{};
    bool resolved_plan_mismatch{};
    bool topology_request_mismatch{};
    bool experiment_mismatch{};
    bool kernel_execution_inputs_mismatch{};
    bool measurement_inputs_mismatch{};
    bool worker_count_mismatch{};
    bool worker_identity_mismatch{};
    bool worker_region_invariant_mismatch{};
    bool service_reservation_mismatch{};
};

[[nodiscard]] auto classify_calibration_only_compatibility(
    const CalibrationOnlyCompatibilityFacts&) noexcept -> std::expected<void, core::Error>;

[[nodiscard]] auto
classify_calibration_only_compatibility_fallible(const CalibrationOnlyCompatibilityFacts&)
    -> std::expected<void, core::Error>;

struct CalibrationOnlyProductionAdmissionAccess final {
    [[nodiscard]] static auto create(
        ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
        ipc::RequestId second_id, std::uint64_t resolved_comparison_order_seed,
        const RunnerContext& context) noexcept
        -> std::expected<CalibrationOnlyProductionAdmission, core::Error>;
};
struct HostCalibrationOnlyProductionAdmissionAccess final {
    [[nodiscard]] static auto
    create_fallible(ipc::PlacementRequest first, ipc::PlacementRequest second,
                    ipc::RequestId first_id, ipc::RequestId second_id, std::uint64_t seed,
                    const RunnerContext&, bool source_route, std::optional<core::Error>* = nullptr)
        -> std::expected<CalibrationOnlyProductionAdmission, core::Error>;
    [[nodiscard]] static auto create(
        ipc::PlacementRequest first, ipc::PlacementRequest second, ipc::RequestId first_id,
        ipc::RequestId second_id, std::uint64_t resolved_comparison_order_seed,
        const RunnerContext& context) noexcept
        -> std::expected<CalibrationOnlyProductionAdmission, core::Error>;
};

} // namespace epycbench::runtime::detail
