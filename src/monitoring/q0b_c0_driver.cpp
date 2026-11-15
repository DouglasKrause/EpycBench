#include "epycbench/monitoring/q0b_c0_driver.hpp"

#include "epycbench/monitoring/q0b_family_characterization.hpp"

#include <exception>
#include <nlohmann/json.hpp>
#include <string_view>
#include <utility>

namespace epycbench::monitoring::qualification {
namespace {

auto error(std::string_view classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q0b_c0_driver.invalid_input"},
            .message = "MON-007 Q0b C0 driver input is inconsistent",
            .operation = "monitoring.run_q0b_c0_calibrations",
            .os_error = std::nullopt,
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto failure(core::Error primary) -> Q0bC0DriverFailure {
    return {std::move(primary), {}};
}

auto failure(runtime::CalibrationOnlyOperationFailure value) -> Q0bC0DriverFailure {
    return {std::move(value.primary), std::move(value.secondary)};
}

auto append_failure(Q0bC0DriverFailure& destination,
                    runtime::CalibrationOnlyOperationFailure source) -> void {
    destination.secondary.push_back(std::move(source.primary));
    for (auto& secondary : source.secondary)
        destination.secondary.push_back(std::move(secondary));
}

auto equal_policy(const measurement::CalibrationDurationPolicy& left,
                  const measurement::CalibrationDurationPolicy& right) noexcept -> bool {
    return left.minimum_duration() == right.minimum_duration() &&
           left.target_duration() == right.target_duration() &&
           left.maximum_duration() == right.maximum_duration();
}
auto equal_service_plan(const runtime::ServicePlacementPlan& left,
                        const runtime::ServicePlacementPlan& right) -> bool {
    return std::ranges::equal(left.assignments(), right.assignments()) &&
           std::ranges::equal(left.reserved_cores(), right.reserved_cores()) &&
           std::ranges::equal(left.reserved_processing_units(), right.reserved_processing_units()) &&
           std::ranges::equal(left.reserved_linux_cpus(), right.reserved_linux_cpus()) &&
           std::ranges::equal(left.unassigned_processing_units(), right.unassigned_processing_units()) &&
           std::ranges::equal(left.unassigned_linux_cpus(), right.unassigned_linux_cpus());
}

template <typename Calibrate, typename Project>
auto run_family(const runtime::CalibrationOnlyProductionOperations& operations,
                const Q0bC0FamilyInput& family, Calibrate calibrate, Project project)
    -> std::expected<nlohmann::ordered_json, Q0bC0DriverFailure> {
    auto prepared = operations.prepare_run(family.inputs);
    if (!prepared)
        return std::unexpected{failure(std::move(prepared.error()))};

    auto calibrated = calibrate(*prepared, family.calibration_policy);
    auto finalized = std::move(*prepared).finalize();
    if (!calibrated) {
        auto result = failure(std::move(calibrated.error()));
        if (!finalized)
            append_failure(result, std::move(finalized.error()));
        return std::unexpected{std::move(result)};
    }
    if (!finalized)
        return std::unexpected{failure(std::move(finalized.error()))};

    auto projected = project(family.admission, family.base, *calibrated);
    if (!projected)
        return std::unexpected{failure(std::move(projected.error()))};
    (*projected)["finalization"] = nlohmann::ordered_json::object(
        {{"operation", "PairwiseProductionRun::finalize"}, {"disposition", "succeeded"}});
    return std::move(*projected);
}

} // namespace

auto run_q0b_c0_calibrations(const runtime::CalibrationOnlyProductionOperations& operations,
                             const Q0bC0FamilyInput& bandwidth,
                             const Q0bC0FamilyInput& latency) noexcept
    -> std::expected<Q0bC0Calibrations, Q0bC0DriverFailure> {
    try {
        if (!equal_policy(bandwidth.calibration_policy, latency.calibration_policy))
            return std::unexpected{failure(error("calibration_policy_mismatch"))};
        if (bandwidth.base.digest() != latency.base.digest() ||
            bandwidth.base.canonical_json() != latency.base.canonical_json())
            return std::unexpected{failure(error("base_environment_mismatch"))};
        const auto& bandwidth_plan = bandwidth.inputs.service_placement();
        const auto& latency_plan = latency.inputs.service_placement();
        if (bandwidth_plan.assignments().size() != 1U ||
            bandwidth_plan.assignments()[0].role != runtime::ServiceRole::control_monitor ||
            !equal_service_plan(bandwidth_plan, latency_plan))
            return std::unexpected{failure(error("service_plan_mismatch"))};

        auto bandwidth_result = run_family(
            operations, bandwidth,
            [](runtime::CalibrationOnlyProductionRun& run,
               const measurement::CalibrationDurationPolicy& policy) {
                return run.calibrate_bandwidth(policy);
            },
            [](const auto& admission, const auto& base, const auto& outcome) {
                return project_q0b_family_calibration(admission, base, outcome);
            });
        if (!bandwidth_result)
            return std::unexpected{std::move(bandwidth_result.error())};

        auto latency_result = run_family(
            operations, latency,
            [](runtime::CalibrationOnlyProductionRun& run,
               const measurement::CalibrationDurationPolicy& policy) {
                return run.calibrate_latency(policy);
            },
            [](const auto& admission, const auto& base, const auto& outcome) {
                return project_q0b_family_calibration(admission, base, outcome);
            });
        if (!latency_result)
            return std::unexpected{std::move(latency_result.error())};

        auto families = nlohmann::ordered_json::array();
        families.push_back(std::move(*bandwidth_result));
        families.push_back(std::move(*latency_result));
        return Q0bC0Calibrations{std::move(families), bandwidth_plan.assignments()[0]};
    } catch (const std::exception&) {
        return std::unexpected{failure(error("contained_exception"))};
    } catch (...) {
        return std::unexpected{failure(error("contained_unknown_failure"))};
    }
}

} // namespace epycbench::monitoring::qualification
