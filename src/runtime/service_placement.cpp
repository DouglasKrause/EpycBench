#include "epycbench/runtime/service_placement.hpp"
#include "core/json_ownership.hpp"
#include "pairwise_v2_contextual_admission_classification.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

namespace epycbench::runtime {
namespace {

[[nodiscard]] auto service_error(std::string code, std::string_view classification,
                                 const core::Error* underlying = nullptr) -> core::Error {
    nlohmann::json context = core::detail::json_object<nlohmann::json>([&](auto& json_value_86) {
        core::detail::set_json(json_value_86, "service_phase", nlohmann::json("assignment"));
        core::detail::set_json(json_value_86, "classification", nlohmann::json(classification));
    });
    core::detail::JsonCleanup context_cleanup{context};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Service placement planning failed",
            .operation = "service_placement",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

template <typename T> [[nodiscard]] auto sorted_unique(std::vector<T> values) -> bool {
    std::ranges::sort(values);
    return std::ranges::adjacent_find(values) == values.end();
}

} // namespace

ServicePlacementPlan::ServicePlacementPlan(
    std::vector<ServiceThreadAssignment> assignments, std::vector<core::CoreId> cores,
    std::vector<core::ProcessingUnitId> processing_units, std::vector<core::LinuxCpuId> linux_cpus,
    std::vector<core::ProcessingUnitId> unassigned_processing_units,
    std::vector<core::LinuxCpuId> unassigned_linux_cpus) noexcept
    : assignments_{std::move(assignments)}, reserved_cores_{std::move(cores)},
      reserved_processing_units_{std::move(processing_units)},
      reserved_linux_cpus_{std::move(linux_cpus)},
      unassigned_processing_units_{std::move(unassigned_processing_units)},
      unassigned_linux_cpus_{std::move(unassigned_linux_cpus)} {}

auto detail::PairwiseV2AdmissionClassificationAccess::create_service_placement(
    const AdmittedPlacementRequest& request, bool measurement_monitor_required) noexcept
    -> std::expected<ServicePlacementPlan,
                     detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    try {
        return create_service_placement_fallible(request, measurement_monitor_required);
    } catch (const std::bad_alloc&) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::allocation,
            service_error("runtime.service_threads.contained_exception", "contained_exception")}};
    } catch (const std::exception&) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::service_placement_contained_exception,
            service_error("runtime.service_threads.contained_exception", "contained_exception")}};
    } catch (...) {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::service_placement_contained_unknown_failure,
            service_error("runtime.service_threads.contained_unknown_failure",
                          "contained_unknown_failure")}};
    }
}
auto detail::PairwiseV2AdmissionClassificationAccess::create_service_placement_fallible(
    const AdmittedPlacementRequest& request, bool measurement_monitor_required)
    -> std::expected<ServicePlacementPlan, detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
    using Branch = detail::PairwiseV2ContextualAdmissionBranch;
    const auto mismatch = [](core::Error error)
        -> std::expected<ServicePlacementPlan, detail::PairwiseV2LegacyClassifiedAdmissionFailure> {
        return std::unexpected{detail::PairwiseV2LegacyClassifiedAdmissionFailure{
            Branch::service_placement_mismatch, std::move(error)}};
    };
    auto cores = std::vector(request.service().cores().begin(), request.service().cores().end());
    auto pus = std::vector(request.service().effective_processing_units().begin(),
                           request.service().effective_processing_units().end());
    auto cpus =
        std::vector(request.service().linux_cpus().begin(), request.service().linux_cpus().end());
    if (!sorted_unique(cores) || !sorted_unique(pus) || !sorted_unique(cpus))
        return mismatch(service_error("runtime.service_threads.invariant_failed",
                                      "duplicate_service_assignment"));
    std::ranges::sort(cores);
    std::ranges::sort(pus);
    std::ranges::sort(cpus);
    if (cpus.empty())
        return mismatch(service_error("runtime.service_threads.assignment_failed",
                                      "insufficient_service_capacity"));
    if (pus.size() != cpus.size())
        return mismatch(service_error("runtime.service_threads.invariant_failed",
                                      "inconsistent_service_reservation"));

    const auto assigned_cpu = cpus.front();
    const auto& topology = request.context().topology();
    const auto pu_it = std::ranges::find(topology.processing_units(), assigned_cpu,
                                         &topology::ProcessingUnit::linux_cpu_id);
    if (pu_it == topology.processing_units().end() ||
        std::ranges::find(pus, pu_it->id) == pus.end() ||
        std::ranges::find(cores, pu_it->core_id) == cores.end())
        return mismatch(service_error("runtime.service_threads.invariant_failed",
                                      "inconsistent_service_reservation"));
    const auto stable_pu = request.context().stable_ids().processing_unit_id(pu_it->id);
    const auto stable_core = request.context().stable_ids().core_id(pu_it->core_id);
    if (!stable_pu || !stable_core)
        return mismatch(service_error("runtime.service_threads.invariant_failed",
                                      "missing_stable_service_identity"));

    for (const auto& worker : request.workers()) {
        if (worker.processing_unit() == pu_it->id || worker.core() == pu_it->core_id)
            return mismatch(service_error("runtime.service_threads.invariant_failed",
                                          "worker_service_conflict"));
    }

    std::vector<ServiceThreadAssignment> assignments;
    assignments.push_back({ServiceRole::control_monitor, stable_pu->get(), stable_core->get(),
                           pu_it->id, pu_it->core_id, assigned_cpu});
    std::optional<core::LinuxCpuId> measurement_cpu;
    if (measurement_monitor_required) {
        std::vector<const topology::ProcessingUnit*> candidates;
        for (const auto cpu : cpus) {
            const auto candidate = std::ranges::find(topology.processing_units(), cpu,
                                                     &topology::ProcessingUnit::linux_cpu_id);
            if (candidate == topology.processing_units().end() ||
                std::ranges::find(pus, candidate->id) == pus.end())
                return mismatch(service_error("runtime.service_threads.invariant_failed",
                                              "inconsistent_service_reservation"));
            if (candidate->core_id == pu_it->core_id)
                continue;
            candidates.push_back(&*candidate);
        }
        std::ranges::sort(candidates, {}, [](const topology::ProcessingUnit* candidate) {
            return std::pair{candidate->core_id, candidate->linux_cpu_id};
        });
        if (candidates.empty())
            return mismatch(service_error("runtime.service_threads.assignment_failed",
                                          "insufficient_service_capacity"));
        const auto* candidate = candidates.front();
        const auto candidate_stable_pu =
            request.context().stable_ids().processing_unit_id(candidate->id);
        const auto candidate_stable_core =
            request.context().stable_ids().core_id(candidate->core_id);
        if (!candidate_stable_pu || !candidate_stable_core)
            return mismatch(service_error("runtime.service_threads.invariant_failed",
                                          "missing_stable_service_identity"));
        assignments.push_back({ServiceRole::measurement_monitor, candidate_stable_pu->get(),
                               candidate_stable_core->get(), candidate->id, candidate->core_id,
                               candidate->linux_cpu_id});
        measurement_cpu = candidate->linux_cpu_id;
    }
    std::vector<core::LinuxCpuId> unassigned_cpus;
    for (const auto cpu : cpus)
        if (cpu != assigned_cpu && (!measurement_cpu || cpu != *measurement_cpu))
            unassigned_cpus.push_back(cpu);
    std::vector<core::ProcessingUnitId> unassigned_pus;
    for (const auto cpu : unassigned_cpus) {
        const auto found = std::ranges::find(topology.processing_units(), cpu,
                                             &topology::ProcessingUnit::linux_cpu_id);
        if (found == topology.processing_units().end() ||
            std::ranges::find(pus, found->id) == pus.end())
            return mismatch(service_error("runtime.service_threads.invariant_failed",
                                          "inconsistent_service_reservation"));
        unassigned_pus.push_back(found->id);
    }
    return ServicePlacementPlan{
        std::move(assignments), std::move(cores),          std::move(pus),
        std::move(cpus),        std::move(unassigned_pus), std::move(unassigned_cpus)};
}

auto detail::PairwiseV2AdmissionClassificationAccess::create_service_placement_source(
    const AdmittedPlacementRequest& request, bool measurement)
    -> std::expected<ServicePlacementPlan, core::Error> {
    auto result = create_service_placement_fallible(request, measurement);
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

auto ServicePlacementPlan::create(const AdmittedPlacementRequest& request,
                                  bool measurement_monitor_required) noexcept
    -> std::expected<ServicePlacementPlan, core::Error> {
    auto result = detail::PairwiseV2AdmissionClassificationAccess::create_service_placement(
        request, measurement_monitor_required);
    if (!result)
        return std::unexpected{std::move(result.error().legacy_)};
    return std::move(*result);
}

auto ServicePlacementPlan::assignments() const noexcept
    -> std::span<const ServiceThreadAssignment> {
    return assignments_;
}
auto ServicePlacementPlan::reserved_cores() const noexcept -> std::span<const core::CoreId> {
    return reserved_cores_;
}
auto ServicePlacementPlan::reserved_processing_units() const noexcept
    -> std::span<const core::ProcessingUnitId> {
    return reserved_processing_units_;
}
auto ServicePlacementPlan::reserved_linux_cpus() const noexcept
    -> std::span<const core::LinuxCpuId> {
    return reserved_linux_cpus_;
}
auto ServicePlacementPlan::unassigned_processing_units() const noexcept
    -> std::span<const core::ProcessingUnitId> {
    return unassigned_processing_units_;
}
auto ServicePlacementPlan::unassigned_linux_cpus() const noexcept
    -> std::span<const core::LinuxCpuId> {
    return unassigned_linux_cpus_;
}

} // namespace epycbench::runtime
