#include "epycbench/runtime/placement_request_projection.hpp"
#include "epycbench/runtime/runner_context.hpp"
#include "runtime/ipc/placement_request_construction_access.hpp"
#include "runtime/placement_provenance_access.hpp"
#include "runtime/scenario_monitoring_policy_projection.hpp"

#include <algorithm>
#include <limits>

namespace epycbench::runtime {
namespace {
using Json = nlohmann::ordered_json;
using Values = ipc::detail::PlacementRequestConstructionValues;

auto projection_error(std::string code, std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Placement request projection failed",
            .operation = "project_placement_request",
            .os_error = std::nullopt,
            .context = nlohmann::json{{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto incoherent() -> core::Error {
    return projection_error("runtime.placement_request_projection.projection_incoherent",
                            "projection_incoherent");
}

auto ratio(const Json& value) -> ipc::detail::RatioValues {
    return {.numerator = value.at("numerator").get<std::uint64_t>(),
            .denominator = value.at("denominator").get<std::uint64_t>()};
}

auto placement_policy(scenario::MemoryPolicy value) -> std::optional<ipc::PlacementPolicy> {
    switch (value) {
    case scenario::MemoryPolicy::bind:
        return ipc::PlacementPolicy::bind;
    }
    return std::nullopt;
}

auto page_mode(scenario::PageMode value) -> std::optional<ipc::PlacementPageMode> {
    switch (value) {
    case scenario::PageMode::base:
        return ipc::PlacementPageMode::base;
    }
    return std::nullopt;
}

auto measurement(const Json& root, scenario::MatrixComparisonOrder order)
    -> std::expected<ipc::detail::MeasurementValues, core::Error> {
    const auto& value = root.at("measurement");
    const auto& observation = value.at("observation");
    const auto& calibration = value.at("calibration");
    const auto& warmup = value.at("warmup");
    const auto& repetition = value.at("repetition");
    const auto& stopping = value.at("stopping");
    if (calibration.at("strategy") != "geometric-refine")
        return std::unexpected{incoherent()};
    return ipc::detail::MeasurementValues{
        core::Duration::from_base_units(
            observation.at("target_duration").at("value").get<std::uint64_t>()),
        core::Duration::from_base_units(
            observation.at("minimum_duration").at("value").get<std::uint64_t>()),
        core::Duration::from_base_units(
            observation.at("maximum_duration").at("value").get<std::uint64_t>()),
        ratio(observation.at("maximum_harness_overhead")),
        calibration.at("enabled").get<bool>(),
        ipc::PlacementCalibrationStrategy::geometric_refine,
        warmup.at("minimum").get<std::uint64_t>(),
        warmup.at("maximum").get<std::uint64_t>(),
        warmup.at("stability_window").get<std::uint64_t>(),
        ratio(warmup.at("stability_threshold")),
        repetition.at("minimum_blocks").get<std::uint64_t>(),
        repetition.at("maximum_blocks").get<std::uint64_t>(),
        repetition.at("minimum_valid_observations_per_variant").get<std::uint64_t>(),
        stopping.at("enabled").get<bool>(),
        ratio(stopping.at("confidence")),
        ratio(stopping.at("ratio_interval_relative_half_width")),
        order,
        root.at("random").at("seed").get<std::uint64_t>()};
}

auto project_values(const scenario::ScenarioDigestedPlan& plan,
                               const scenario::MatrixCellId& cell_id) noexcept
    -> std::expected<Values, core::Error> {
    try {
        const auto& expanded = plan.matrix_expanded();
        const scenario::ExpandedMatrixExperiment* matrix = nullptr;
        const scenario::ResolvedMatrixCell* cell = nullptr;
        for (const auto& candidate : expanded.matrix_experiments()) {
            const auto found =
                std::ranges::find(candidate.cells, cell_id, &scenario::ResolvedMatrixCell::id);
            if (found != candidate.cells.end()) {
                matrix = &candidate;
                cell = &*found;
                break;
            }
        }
        if (cell == nullptr)
            return std::unexpected{projection_error(
                "runtime.placement_request_projection.selection_failed", "selection_not_found")};
        if (cell->worker_row_index >= matrix->worker_rows.size() ||
            matrix->experiment_index > std::numeric_limits<std::uint64_t>::max())
            return std::unexpected{incoherent()};

        const auto& dispatched = expanded.kernel_dispatched();
        const auto binding =
            std::ranges::find(dispatched.experiment_workloads(), matrix->experiment_index,
                              &scenario::ExperimentWorkloadBinding::experiment_index);
        if (binding == dispatched.experiment_workloads().end())
            return std::unexpected{incoherent()};
        const auto workload = dispatched.workloads().find(binding->workload_id);
        if (workload == dispatched.workloads().end())
            return std::unexpected{incoherent()};
        auto experiment_id = scenario::ScenarioIdentifier::parse(matrix->experiment_id);
        auto workload_id = scenario::ScenarioIdentifier::parse(binding->workload_id);
        if (!experiment_id || !workload_id)
            return std::unexpected{incoherent()};

        const auto& service = dispatched.memory_placement_resolved().service_cpu_resolved();
        const auto& resources = service.resource_resolved();
        auto measured = measurement(resources.default_expanded().value(), matrix->comparison.order);
        auto monitored = detail::project_scenario_monitoring_inputs(resources.default_expanded().value());
        if (!measured || !monitored)
            return std::unexpected{incoherent()};

        std::optional<ipc::detail::KernelValues> kernel_values;
        if (const auto* bandwidth = std::get_if<kernel::SequentialReadBandwidthConfig>(
                &workload->second.configuration)) {
            if (binding->pointer_cycle_seed || binding->pointer_cycle_seed_origin)
                return std::unexpected{incoherent()};
            kernel_values.emplace(
                ipc::detail::BandwidthValues{*bandwidth, std::nullopt, std::nullopt});
        } else {
            const auto* latency =
                std::get_if<kernel::DependentLoadLatencyConfig>(&workload->second.configuration);
            if (latency == nullptr || !binding->pointer_cycle_seed ||
                !binding->pointer_cycle_seed_origin)
                return std::unexpected{incoherent()};
            kernel_values.emplace(ipc::detail::LatencyValues{*latency, *binding->pointer_cycle_seed,
                                                             *binding->pointer_cycle_seed_origin});
        }

        Values values{monitored->has_value() ? ipc::PlacementRequestTransportVersion::v3
                                             : ipc::PlacementRequestTransportVersion::v2,
                      detail::PlacementProvenanceConstructionAccess::scenario(
                          plan.scenario_digest(), plan.resolved_scenario_plan_digest()),
                      resources.topology_fingerprints().physical,
                      resources.topology_fingerprints().effective,
                      static_cast<std::uint64_t>(matrix->experiment_index),
                      std::move(*experiment_id),
                      std::move(*workload_id),
                      workload->second.kernel_id,
                      std::move(*kernel_values),
                      std::move(*measured),
                      std::move(*monitored),
                      {},
                      service.reserved_service_cores(),
                      {}};
        values.service_cpus.assign(service.reserved_service_cpus().begin(),
                                   service.reserved_service_cpus().end());

        const auto region_bytes = std::visit(
            [](const auto& configuration) {
                using Configuration = std::remove_cvref_t<decltype(configuration)>;
                if constexpr (std::same_as<Configuration, kernel::SequentialReadBandwidthConfig>)
                    return configuration.working_set_per_worker();
                else
                    return configuration.working_set();
            },
            workload->second.configuration);
        const auto& placement = cell->memory_placement;
        const auto projected_policy = placement_policy(placement.policy);
        const auto projected_page_mode = page_mode(placement.page_mode);
        if (!projected_policy || !projected_page_mode)
            return std::unexpected{incoherent()};
        for (const auto& worker : matrix->worker_rows[cell->worker_row_index].workers) {
            ipc::detail::RegionValues region{region_bytes,
                                             *projected_policy,
                                             {},
                                             *projected_page_mode,
                                             placement.page_size,
                                             {placement.placement_threshold.numerator(),
                                              placement.placement_threshold.denominator()},
                                             placement.prefault,
                                             placement.verify_before,
                                             placement.verify_after};
            for (const auto& node : placement.nodes)
                region.nodes.push_back({node.stable_id, node.linux_node});
            values.workers.push_back(
                {worker.stable_id, worker.core_id, worker.linux_cpu, std::move(region)});
        }
        return values;
    } catch (...) {
        return std::unexpected{incoherent()};
    }
}

auto project_values(const scenario::ScenarioDigestedPlan& plan,
                               const scenario::ComparisonVariantId& variant_id) noexcept
    -> std::expected<Values, core::Error> {
    try {
        const auto& expanded = plan.matrix_expanded();
        const scenario::ResolvedComparisonExperiment* experiment = nullptr;
        const scenario::ResolvedComparisonVariant* variant = nullptr;
        std::size_t matches = 0;
        for (const auto& candidate : expanded.comparison_experiments()) {
            if (candidate.experiment_id != variant_id.experiment_id)
                continue;
            for (const auto& candidate_variant : candidate.variants) {
                if (candidate_variant.variant_id == variant_id.variant_id) {
                    ++matches;
                    experiment = &candidate;
                    variant = &candidate_variant;
                }
            }
        }
        if (matches == 0)
            return std::unexpected{projection_error(
                "runtime.placement_request_projection.selection_failed", "selection_not_found")};
        if (matches != 1 || experiment == nullptr || variant == nullptr ||
            experiment->experiment_index > std::numeric_limits<std::uint64_t>::max())
            return std::unexpected{incoherent()};

        const auto& dispatched = expanded.kernel_dispatched();
        const auto binding = std::ranges::find(
            dispatched.experiment_workloads(), experiment->experiment_index,
            &scenario::ExperimentWorkloadBinding::experiment_index);
        const auto workload = dispatched.workloads().find(experiment->workload_id);
        if (binding == dispatched.experiment_workloads().end() ||
            workload == dispatched.workloads().end() ||
            binding->workload_id != experiment->workload_id)
            return std::unexpected{incoherent()};
        auto experiment_id = scenario::ScenarioIdentifier::parse(experiment->experiment_id);
        auto workload_id = scenario::ScenarioIdentifier::parse(experiment->workload_id);
        if (!experiment_id || !workload_id)
            return std::unexpected{incoherent()};

        const auto& service = dispatched.memory_placement_resolved().service_cpu_resolved();
        const auto& resources = service.resource_resolved();
        auto measured = measurement(resources.default_expanded().value(), experiment->comparison.order);
        auto monitored = detail::project_scenario_monitoring_inputs(resources.default_expanded().value());
        if (!measured || !monitored)
            return std::unexpected{incoherent()};

        std::optional<ipc::detail::KernelValues> kernel_values;
        if (const auto* bandwidth = std::get_if<kernel::SequentialReadBandwidthConfig>(
                &workload->second.configuration)) {
            if (binding->pointer_cycle_seed || binding->pointer_cycle_seed_origin)
                return std::unexpected{incoherent()};
            kernel_values.emplace(ipc::detail::BandwidthValues{*bandwidth, std::nullopt,
                                                                std::nullopt});
        } else {
            const auto* latency =
                std::get_if<kernel::DependentLoadLatencyConfig>(&workload->second.configuration);
            if (latency == nullptr || !binding->pointer_cycle_seed ||
                !binding->pointer_cycle_seed_origin)
                return std::unexpected{incoherent()};
            kernel_values.emplace(ipc::detail::LatencyValues{*latency, *binding->pointer_cycle_seed,
                                                             *binding->pointer_cycle_seed_origin});
        }

        Values values{monitored->has_value() ? ipc::PlacementRequestTransportVersion::v3
                                             : ipc::PlacementRequestTransportVersion::v2,
                      detail::PlacementProvenanceConstructionAccess::scenario(
                          plan.scenario_digest(), plan.resolved_scenario_plan_digest()),
                      resources.topology_fingerprints().physical,
                      resources.topology_fingerprints().effective,
                      static_cast<std::uint64_t>(experiment->experiment_index),
                      std::move(*experiment_id),
                      std::move(*workload_id),
                      workload->second.kernel_id,
                      std::move(*kernel_values),
                      std::move(*measured),
                      std::move(*monitored),
                      {},
                      service.reserved_service_cores(),
                      {}};
        values.service_cpus.assign(service.reserved_service_cpus().begin(),
                                   service.reserved_service_cpus().end());
        const auto region_bytes = std::visit(
            [](const auto& configuration) {
                using Configuration = std::remove_cvref_t<decltype(configuration)>;
                if constexpr (std::same_as<Configuration, kernel::SequentialReadBandwidthConfig>)
                    return configuration.working_set_per_worker();
                else
                    return configuration.working_set();
            },
            workload->second.configuration);
        auto projected = std::visit(
            [&](const auto& placement) -> std::expected<void, core::Error> {
                const auto projected_policy = placement_policy(placement.policy);
                const auto projected_page_mode = page_mode(placement.page_mode);
                if (!projected_policy || !projected_page_mode)
                    return std::unexpected{incoherent()};
                if constexpr (std::same_as<std::remove_cvref_t<decltype(placement)>,
                                           scenario::ResolvedPerWorkerMemoryPlacement>) {
                    if (placement.worker_nodes.size() != experiment->workers.size())
                        return std::unexpected{incoherent()};
                }
                for (std::size_t index = 0; index < experiment->workers.size(); ++index) {
                    const auto& worker = experiment->workers[index];
                    ipc::detail::RegionValues region{
                        region_bytes, *projected_policy, {}, *projected_page_mode,
                        placement.page_size,
                        {placement.placement_threshold.numerator(),
                         placement.placement_threshold.denominator()},
                        placement.prefault, placement.verify_before, placement.verify_after};
                    if constexpr (std::same_as<std::remove_cvref_t<decltype(placement)>,
                                               scenario::ResolvedPerWorkerMemoryPlacement>) {
                        const auto& mapping = placement.worker_nodes[index];
                        if (mapping.worker != worker)
                            return std::unexpected{incoherent()};
                        region.nodes.push_back({mapping.node.stable_id,
                                                mapping.node.linux_node});
                    } else {
                        for (const auto& node : placement.nodes)
                            region.nodes.push_back({node.stable_id, node.linux_node});
                    }
                    values.workers.push_back(
                        {worker.stable_id, worker.core_id, worker.linux_cpu,
                         std::move(region)});
                }
                return {};
            }, variant->memory_placement);
        if (!projected) return std::unexpected{projected.error()};
        return values;
    } catch (...) {
        return std::unexpected{incoherent()};
    }
}
// The Scenario's selected PUs remain the role-eligible pool. Only the reservation
// expands; contextual admission still verifies the complete effective core closure.
auto close_service_reservation(Values& values, const RunnerContext& context)
    -> std::expected<void, core::Error> {
    if (values.physical != context.fingerprints().physical ||
        values.effective != context.fingerprints().effective)
        return std::unexpected{projection_error(
            "runtime.placement_request_projection.context_mismatch", "topology_fingerprint_mismatch")};
    const auto& topology = context.topology();
    const auto& effective = topology.machine().effective_cpus.value();
    if (!effective)
        return std::unexpected{incoherent()};
    std::vector<core::LinuxCpuId> closure;
    for (const auto& stable_core : values.service_cores) {
        const auto core_id = context.stable_ids().core(stable_core);
        if (!core_id || !topology.core(*core_id))
            return std::unexpected{incoherent()};
        bool has_effective_member = false;
        for (const auto pu_id : topology.core_members(*core_id)) {
            const auto pu = topology.processing_unit(pu_id);
            if (!pu)
                return std::unexpected{incoherent()};
            if (effective->contains(pu->linux_cpu_id)) {
                has_effective_member = true;
                closure.push_back(pu->linux_cpu_id);
            }
        }
        if (!has_effective_member)
            return std::unexpected{incoherent()};
    }
    std::ranges::sort(closure);
    if (std::ranges::adjacent_find(closure) != closure.end())
        return std::unexpected{incoherent()};
    for (const auto selected : values.service_cpus)
        if (!std::ranges::binary_search(closure, selected))
            return std::unexpected{incoherent()};
    values.service_cpus = std::move(closure);
    return {};
}

} // namespace

auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                               const scenario::MatrixCellId& cell_id) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error> {
    try {
        auto values = project_values(plan, cell_id);
        if (!values)
            return std::unexpected{std::move(values.error())};
        return ipc::detail::PlacementRequestConstructionAccess::construct(std::move(*values));
    } catch (...) {
        return std::unexpected{incoherent()};
    }
}

auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                               const scenario::MatrixCellId& cell_id,
                               const RunnerContext& context) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error> {
    try {
        auto values = project_values(plan, cell_id);
        if (!values)
            return std::unexpected{std::move(values.error())};
        auto closed = close_service_reservation(*values, context);
        if (!closed)
            return std::unexpected{std::move(closed.error())};
        return ipc::detail::PlacementRequestConstructionAccess::construct(std::move(*values));
    } catch (...) {
        return std::unexpected{incoherent()};
    }
}

auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                               const scenario::ComparisonVariantId& variant_id) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error> {
    try {
        auto values = project_values(plan, variant_id);
        if (!values)
            return std::unexpected{std::move(values.error())};
        return ipc::detail::PlacementRequestConstructionAccess::construct(std::move(*values));
    } catch (...) {
        return std::unexpected{incoherent()};
    }
}

auto project_placement_request(const scenario::ScenarioDigestedPlan& plan,
                               const scenario::ComparisonVariantId& variant_id,
                               const RunnerContext& context) noexcept
    -> std::expected<ipc::PlacementRequest, core::Error> {
    try {
        auto values = project_values(plan, variant_id);
        if (!values)
            return std::unexpected{std::move(values.error())};
        auto closed = close_service_reservation(*values, context);
        if (!closed)
            return std::unexpected{std::move(closed.error())};
        return ipc::detail::PlacementRequestConstructionAccess::construct(std::move(*values));
    } catch (...) {
        return std::unexpected{incoherent()};
    }
}

} // namespace epycbench::runtime
