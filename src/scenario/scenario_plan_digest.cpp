#include "epycbench/scenario/scenario_plan_digest.hpp"
#include "core/json_ownership.hpp"
#include "scenario/json_cleanup.hpp"
#include "scenario/retained_plan_projection.hpp"

#include "epycbench/kernel/configuration.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <ranges>
#include <set>
#include <utility>

namespace epycbench::scenario {
namespace {

using Json = nlohmann::ordered_json;
using detail::append_json;
using detail::append_object_json;
using detail::clone_json;
using detail::json_object;
using detail::JsonCleanup;
using detail::JsonPointerCleanup;
using detail::set_json;

auto error(std::string classification, std::string phase = {}, std::string underlying = {})
    -> core::Error {
    auto context = nlohmann::json::object();
    JsonCleanup context_cleanup{context};
    context["classification"] = std::move(classification);
    if (!phase.empty())
        context["phase"] = std::move(phase);
    if (!underlying.empty())
        context["underlying_code"] = std::move(underlying);
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{"scenario.digest.internal_failure"},
            .message = "Scenario digest construction failed",
            .operation = "compute_scenario_plan_digests",
            .os_error = std::nullopt,
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

auto decimal(std::uint64_t value) -> std::string { return std::to_string(value); }

auto ratio_json(const ExactRatio& ratio) -> Json {
    return json_object([&](Json& json_target_0) {
        set_json(json_target_0, "numerator", decimal(ratio.numerator()));
        set_json(json_target_0, "denominator", decimal(ratio.denominator()));
    });
}

auto worker_json(const ResolvedProcessingUnit& worker) -> Json {
    return json_object([&](Json& json_target_1) {
        set_json(json_target_1, "stable_pu", worker.stable_id.value());
        set_json(json_target_1, "stable_core", worker.core_id.value());
        set_json(json_target_1, "linux_cpu", worker.linux_cpu.value());
    });
}

auto service_worker_json(const ResolvedServiceCpu& worker) -> Json {
    return json_object([&](Json& json_target_2) {
        set_json(json_target_2, "stable_pu", worker.stable_id.value());
        set_json(json_target_2, "stable_core", worker.core_id.value());
        set_json(json_target_2, "linux_cpu", worker.linux_cpu.value());
    });
}

auto node_json(const ResolvedMemoryNode& node) -> Json {
    return json_object([&](Json& json_target_3) {
        set_json(json_target_3, "stable_id", node.stable_id.value());
        set_json(json_target_3, "linux_node", node.linux_node.value());
    });
}

auto placement_json(const ResolvedMemoryPlacement& placement) -> Json {
    auto nodes = Json::array();
    JsonCleanup nodes_cleanup{nodes};
    for (const auto& node : placement.nodes)
        append_json(nodes, node_json(node));
    return json_object([&](Json& json_target_4) {
        set_json(json_target_4, "policy", "bind");
        set_json(json_target_4, "nodes", std::move(nodes));
        set_json(json_target_4, "page_mode", "base");
        set_json(json_target_4, "page_size_bytes", decimal(placement.page_size.value()));
        set_json(json_target_4, "placement_threshold", ratio_json(placement.placement_threshold));
        set_json(json_target_4, "prefault", placement.prefault);
        set_json(json_target_4, "verify_before", placement.verify_before);
        set_json(json_target_4, "verify_after", placement.verify_after);
    });
}

auto placement_json(const ResolvedPerWorkerMemoryPlacement& placement) -> Json {
    auto mappings = Json::array();
    JsonCleanup mappings_cleanup{mappings};
    for (const auto& mapping : placement.worker_nodes)
        append_object_json(mappings, [&](Json& json_target_5) {
            set_json(json_target_5, "worker", worker_json(mapping.worker));
            set_json(json_target_5, "node", node_json(mapping.node));
        });
    return json_object([&](Json& json_target_6) {
        set_json(json_target_6, "policy", "bind");
        set_json(json_target_6, "per_worker_nodes", std::move(mappings));
        set_json(json_target_6, "page_mode", "base");
        set_json(json_target_6, "page_size_bytes", decimal(placement.page_size.value()));
        set_json(json_target_6, "placement_threshold", ratio_json(placement.placement_threshold));
        set_json(json_target_6, "prefault", placement.prefault);
        set_json(json_target_6, "verify_before", placement.verify_before);
        set_json(json_target_6, "verify_after", placement.verify_after);
    });
}

auto placement_json(const ComparisonMemoryPlacement& placement) -> Json {
    return std::visit([](const auto& value) { return placement_json(value); }, placement);
}

auto validate_workers(const std::vector<ResolvedProcessingUnit>& workers,
                      const core::CpuSet& mirror) -> bool {
    std::set<core::LinuxCpuId> ids;
    std::set<std::string_view> stable_pus;
    for (const auto& worker : workers) {
        if (!ids.insert(worker.linux_cpu).second ||
            !stable_pus.insert(worker.stable_id.value()).second)
            return false;
    }
    return std::ranges::equal(ids, mirror);
}

auto validate_nodes(const ResolvedMemoryPlacement& placement) -> bool {
    std::set<core::LinuxNumaNodeId> ids;
    std::set<std::string_view> stable;
    for (const auto& node : placement.nodes) {
        if (!ids.insert(node.linux_node).second || !stable.insert(node.stable_id.value()).second)
            return false;
    }
    return ids == placement.linux_nodes && !placement.nodes.empty() &&
           placement.page_size.value() > 0U;
}

auto validate_nodes(const ResolvedPerWorkerMemoryPlacement& placement) -> bool {
    return !placement.worker_nodes.empty() && placement.page_size.value() > 0U &&
           std::ranges::all_of(placement.worker_nodes, [](const auto& mapping) {
               return !mapping.worker.stable_id.value().empty() &&
                      !mapping.node.stable_id.value().empty();
           });
}

auto validate_nodes(const ComparisonMemoryPlacement& placement) -> bool {
    return std::visit([](const auto& value) { return validate_nodes(value); }, placement);
}

auto semantic_intent(const Json& value) -> Json {
    if (value.is_array()) {
        auto result = Json::array();
        JsonCleanup result_cleanup{result};
        for (const auto& entry : value)
            append_json(result, semantic_intent(entry));
        return Json(std::move(result));
    }
    if (!value.is_object())
        return value;
    if (value.value("unit", std::string{}) == "ratio") {
        return json_object([&](Json& json_target_7) {
            set_json(json_target_7, "unit", "ratio");
            set_json(json_target_7, "numerator",
                     decimal(value.at("numerator").get<std::uint64_t>()));
            set_json(json_target_7, "denominator",
                     decimal(value.at("denominator").get<std::uint64_t>()));
        });
    }
    if (value.value("unit", std::string{}) == "nanosecond") {
        return json_object([&](Json& json_target_8) {
            set_json(json_target_8, "unit", "nanosecond");
            set_json(json_target_8, "value", decimal(value.at("value").get<std::uint64_t>()));
        });
    }
    auto result = Json::object();
    JsonCleanup result_cleanup{result};
    for (const auto& [key, entry] : value.items())
        set_json(result, key, semantic_intent(entry));
    return Json(std::move(result));
}

auto default_matrix_memory() -> Json {
    return json_object([&](Json& json_target_9) {
        set_json(json_target_9, "policy", "bind");
        set_json(json_target_9, "page_mode", "base");
        set_json(json_target_9, "placement_threshold", json_object([&](Json& json_target_10) {
                     set_json(json_target_10, "unit", "ratio");
                     set_json(json_target_10, "numerator", "999");
                     set_json(json_target_10, "denominator", "1000");
                 }));
        set_json(json_target_9, "prefault", true);
        set_json(json_target_9, "verify_before", true);
        set_json(json_target_9, "verify_after", true);
    });
}

auto scenario_experiment_json(const Json& experiment) -> Json {
    auto result = semantic_intent(experiment);
    JsonCleanup result_cleanup{result};
    if (core::detail::json_string_equal(result.at("type"), "node-pair-matrix") &&
        !result.contains("memory"))
        set_json(result, "memory", default_matrix_memory());
    return Json(std::move(result));
}

auto workload_json(const DispatchedWorkload& workload) -> std::expected<Json, core::Error> {
    auto projected = kernel::semantic_projection(workload.kernel_id, workload.configuration);
    JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
    if (!projected)
        return std::unexpected{
            error("scenario_projection_failed", "workload", projected.error().code.value())};
    return std::move(*projected);
}

auto binding_for(const detail::RetainedPlanProjectionView& scenario, std::size_t index)
    -> const ExperimentWorkloadBinding* {
    const auto& bindings = scenario.experiment_workloads;
    const auto found = std::ranges::find_if(
        bindings, [index](const auto& b) { return b.experiment_index == index; });
    return found == bindings.end() ? nullptr : &*found;
}

auto worker_reference_for(const detail::RetainedPlanProjectionView& scenario, std::size_t index)
    -> const ExperimentWorkerReference* {
    const auto& refs = scenario.experiment_workers;
    const auto found =
        std::ranges::find_if(refs, [index](const auto& r) { return r.experiment_index == index; });
    return found == refs.end() ? nullptr : &*found;
}

auto seed_json(const ExperimentWorkloadBinding& binding) -> Json {
    if (!binding.pointer_cycle_seed)
        return nullptr;
    return json_object([&](Json& json_target_11) {
        set_json(json_target_11, "value", decimal(*binding.pointer_cycle_seed));
        set_json(json_target_11, "origin",
                 *binding.pointer_cycle_seed_origin == PointerCycleSeedOrigin::explicit_value
                     ? "explicit"
                     : "derived");
    });
}

auto comparison_order_token(MatrixComparisonOrder order) -> std::string_view {
    switch (order) {
    case MatrixComparisonOrder::randomized_abba:
        return "randomized-abba";
    case MatrixComparisonOrder::randomized_baab:
        return "randomized-baab";
    case MatrixComparisonOrder::randomized_balanced:
        return "randomized-balanced";
    }
    return {};
}

auto value_origin_token(ValueOriginKind origin) -> std::string_view {
    switch (origin) {
    case ValueOriginKind::explicit_value:
        return "explicit-value";
    case ValueOriginKind::schema_default:
        return "schema-default";
    case ValueOriginKind::scenario_default:
        return "scenario-default";
    case ValueOriginKind::experiment_override:
        return "experiment-override";
    case ValueOriginKind::variant_override:
        return "variant-override";
    case ValueOriginKind::matrix_override:
        return "matrix-override";
    }
    return "invalid";
}

auto matrix_id_json(const MatrixCellId& id) -> Json {
    return json_object([&](Json& json_target_12) {
        set_json(json_target_12, "experiment", id.experiment_id);
        set_json(json_target_12, "worker_node", id.worker_node.value());
        set_json(json_target_12, "memory_node", id.memory_node.value());
    });
}

auto validate_profiles(const detail::RetainedPlanProjectionView& scenario) -> bool {
    return scenario.expansion_profile == matrix_expansion_profile &&
           scenario.dispatch_profile == kernel_dispatch_profile &&
           scenario.memory_profile == memory_placement_resolution_profile &&
           scenario.service_profile == service_cpu_resolution_profile &&
           scenario.resource_profile == resource_resolution_profile;
}

auto validate_bindings(const detail::RetainedPlanProjectionView& scenario) -> bool {
    const auto& intents = scenario.effective.at("experiments");
    const auto& bindings = scenario.experiment_workloads;
    if (bindings.size() != intents.size())
        return false;
    std::set<std::size_t> indexes;
    for (const auto& binding : bindings) {
        if (binding.experiment_index >= intents.size() ||
            !indexes.insert(binding.experiment_index).second ||
            scenario.workloads.find(binding.workload_id) ==
                scenario.workloads.end() ||
            (binding.pointer_cycle_seed.has_value() !=
             binding.pointer_cycle_seed_origin.has_value()))
            return false;
    }
    return true;
}

auto validate_deferred(const detail::RetainedPlanProjectionView& scenario,
                       const ExpandedMatrixExperiment& matrix,
                       const ExperimentWorkloadBinding& binding) -> bool {
    const auto workload = scenario.workloads.find(binding.workload_id);
    if (workload == scenario.workloads.end())
        return false;
    for (const auto constraint : binding.deferred_constraints) {
        if (constraint == DeferredKernelConstraint::exactly_one_worker &&
            std::ranges::any_of(matrix.worker_rows,
                                [](const auto& row) { return row.workers.size() != 1U; }))
            return false;
        if (constraint == DeferredKernelConstraint::working_set_at_least_resolved_page_size) {
            const auto working_set = std::visit(
                [](const auto& config) {
                    if constexpr (requires { config.working_set(); })
                        return config.working_set();
                    else
                        return config.working_set_per_worker();
                },
                workload->second.configuration);
            if (std::ranges::any_of(matrix.cells, [&](const auto& cell) {
                    return working_set < cell.memory_placement.page_size;
                }))
                return false;
        }
    }
    return true;
}

auto service_json(const detail::RetainedPlanProjectionView& scenario) -> std::expected<Json, core::Error> {
    Json pools = Json::object();
    JsonCleanup pools_cleanup{pools};
    std::set<core::LinuxCpuId> union_cpus;
    std::set<std::string> union_cores;
    for (const auto& [name, pool] : scenario.service_cpu_pools) {
        auto workers = Json::array();
        JsonCleanup workers_cleanup{workers};
        std::set<core::LinuxCpuId> pool_cpus;
        for (const auto& worker : pool.processing_units) {
            if (!pool_cpus.insert(worker.linux_cpu).second)
                return std::unexpected{error("invariant_failed", "service")};
            union_cpus.insert(worker.linux_cpu);
            union_cores.emplace(worker.core_id.value());
            append_json(workers, service_worker_json(worker));
        }
        if (!std::ranges::equal(pool_cpus, pool.linux_cpus))
            return std::unexpected{error("invariant_failed", "service")};
        set_json(pools, name, json_object([&](Json& json_target_13) {
                     set_json(json_target_13, "workers", std::move(workers));
                 }));
    }
    if (!std::ranges::equal(union_cpus, scenario.reserved_service_cpus))
        return std::unexpected{error("invariant_failed", "service")};
    std::vector<std::string> retained_cores;
    for (const auto& id : scenario.reserved_service_cores)
        retained_cores.emplace_back(id.value());
    std::ranges::sort(retained_cores);
    retained_cores.erase(std::unique(retained_cores.begin(), retained_cores.end()),
                         retained_cores.end());
    if (!std::ranges::equal(union_cores, retained_cores))
        return std::unexpected{error("invariant_failed", "service")};
    auto cpus = Json::array();
    JsonCleanup cpus_cleanup{cpus};
    for (const auto cpu : union_cpus)
        cpus.push_back(cpu.value());
    auto cores = Json::array();
    JsonCleanup cores_cleanup{cores};
    for (const auto& core : retained_cores)
        cores.push_back(core);
    return json_object([&](Json& json_target_14) {
        set_json(json_target_14, "dedicated_service_placement",
                 scenario.dedicated_service_placement);
        set_json(json_target_14, "pools", std::move(pools));
        set_json(json_target_14, "reserved_linux_cpus", std::move(cpus));
        set_json(json_target_14, "reserved_stable_cores", std::move(cores));
    });
}

auto comparison_experiment_json(const detail::RetainedPlanProjectionView& scenario, const Json& intent,
                                std::size_t index, const ExperimentWorkloadBinding& binding)
    -> std::expected<Json, core::Error> {
    const auto retained =
        std::ranges::find_if(scenario.comparison_experiments,
                             [index](const auto& item) { return item.experiment_index == index; });
    if (retained == scenario.comparison_experiments.end() ||
        retained->experiment_id != intent.at("id").get<std::string>() ||
        retained->workload_id != binding.workload_id)
        return std::unexpected{error("invariant_failed", "comparison")};
    if (binding.workload_id != intent.at("workload").get<std::string>())
        return std::unexpected{error("invariant_failed", "comparison")};
    const auto* reference = worker_reference_for(scenario, index);
    if (reference == nullptr)
        return std::unexpected{error("invariant_failed", "comparison")};
    const auto workers_found = scenario.worker_sets.find(reference->worker_set_name);
    if (workers_found == scenario.worker_sets.end() ||
        !validate_workers(workers_found->second.processing_units,
                          workers_found->second.linux_cpus) ||
        retained->workers != workers_found->second.processing_units ||
        retained->linux_cpus != workers_found->second.linux_cpus)
        return std::unexpected{error("invariant_failed", "comparison")};
    auto workers = Json::array();
    JsonCleanup workers_cleanup{workers};
    for (const auto& worker : workers_found->second.processing_units)
        append_json(workers, worker_json(worker));

    auto variants = Json::array();
    JsonCleanup variants_cleanup{variants};
    const auto& variant_intents = intent.at("variants");
    if (retained->variants.size() != variant_intents.size())
        return std::unexpected{error("invariant_failed", "comparison")};
    for (std::size_t variant = 0; variant < variant_intents.size(); ++variant) {
        const auto found = std::ranges::find_if(
            scenario.comparison_memory, [index, variant](const auto& item) {
                return item.experiment_index == index && item.variant_index == variant;
            });
        if (found == scenario.comparison_memory.end() ||
            !validate_nodes(found->placement) ||
            retained->variants[variant].variant_index != variant ||
            retained->variants[variant].variant_id !=
                variant_intents.at(variant).at("id").get<std::string>() ||
            retained->variants[variant].memory_placement != found->placement)
            return std::unexpected{error("invariant_failed", "comparison")};
        if (const auto* per_worker =
                std::get_if<ResolvedPerWorkerMemoryPlacement>(&found->placement)) {
            if (per_worker->worker_nodes.size() != retained->workers.size())
                return std::unexpected{error("invariant_failed", "comparison")};
            for (std::size_t worker = 0; worker < retained->workers.size(); ++worker)
                if (per_worker->worker_nodes[worker].worker != retained->workers[worker])
                    return std::unexpected{error("invariant_failed", "comparison")};
        }
        append_object_json(variants, [&](Json& json_target_15) {
            set_json(json_target_15, "variant_index", variant);
            set_json(json_target_15, "variant_id", variant_intents.at(variant).at("id"));
            set_json(json_target_15, "memory", placement_json(found->placement));
        });
    }
    std::vector<std::string> candidates;
    for (const auto& candidate : intent.at("compare").at("candidates"))
        candidates.push_back(candidate.get<std::string>());
    if (retained->baseline_variant_id != intent.at("compare").at("baseline").get<std::string>() ||
        retained->candidate_variant_ids != candidates)
        return std::unexpected{error("invariant_failed", "comparison")};
    const auto& effect = intent.at("compare").at("practical_effect");
    const auto& confidence = intent.at("compare").at("confidence");
    auto expected_effect = ExactRatio::create(effect.at("numerator").get<std::uint64_t>(),
                                              effect.at("denominator").get<std::uint64_t>());
    auto expected_confidence =
        ExactRatio::create(confidence.at("numerator").get<std::uint64_t>(),
                           confidence.at("denominator").get<std::uint64_t>());
    const auto& expected_order = intent.at("compare").at("order").at("strategy");
    const auto expected_order_token = comparison_order_token(retained->comparison.order);
    const auto has_seed = intent.at("compare").at("order").contains("seed");
    if (!expected_effect || !expected_confidence ||
        retained->comparison.practical_effect != *expected_effect ||
        retained->comparison.confidence != *expected_confidence ||
        expected_order != expected_order_token ||
        has_seed != retained->comparison.order_seed.has_value() ||
        (has_seed && intent.at("compare").at("order").at("seed").get<std::uint64_t>() !=
                         *retained->comparison.order_seed))
        return std::unexpected{error("invariant_failed", "comparison")};
    auto comparison_json = semantic_intent(intent.at("compare"));
    JsonCleanup comparison_json_cleanup{comparison_json};
    set_json(comparison_json, "practical_effect_origin", json_object([&](Json& json_target_16) {
                 set_json(json_target_16, "kind",
                          value_origin_token(retained->practical_effect_origin.kind));
                 set_json(json_target_16, "source_pointer",
                          retained->practical_effect_origin.source_pointer
                              ? Json{*retained->practical_effect_origin.source_pointer}
                              : Json{nullptr});
             }));
    return json_object([&](Json& json_target_17) {
        set_json(json_target_17, "experiment_index", index);
        set_json(json_target_17, "experiment_id", intent.at("id"));
        set_json(json_target_17, "type", "comparison");
        set_json(json_target_17, "workload_id", binding.workload_id);
        set_json(json_target_17, "pointer_cycle_seed", seed_json(binding));
        set_json(json_target_17, "workers", std::move(workers));
        set_json(json_target_17, "variants", std::move(variants));
        set_json(json_target_17, "comparison", std::move(comparison_json));
    });
}

auto matrix_experiment_json(const detail::RetainedPlanProjectionView& scenario, const Json& intent,
                            std::size_t index, const ExperimentWorkloadBinding& binding,
                            const ExpandedMatrixExperiment& matrix)
    -> std::expected<Json, core::Error> {
    if (matrix.experiment_index != index ||
        binding.workload_id != intent.at("workload").get<std::string>() ||
        matrix.experiment_id != intent.at("id").get<std::string>() ||
        matrix.worker_rows.size() != matrix.worker_axis.size() ||
        !validate_deferred(scenario, matrix, binding))
        return std::unexpected{error("invariant_failed", "matrix")};
    const auto axis_ordered = [](const auto& axis) {
        return std::ranges::is_sorted(axis, [](const auto& left, const auto& right) {
            if (left.linux_node != right.linux_node)
                return left.linux_node < right.linux_node;
            return left.stable_id < right.stable_id;
        });
    };
    if (!axis_ordered(matrix.worker_axis) || !axis_ordered(matrix.memory_axis) ||
        (!matrix.worker_axis.empty() &&
         matrix.memory_axis.size() >
             std::numeric_limits<std::size_t>::max() / matrix.worker_axis.size()) ||
        matrix.cells.size() != matrix.worker_axis.size() * matrix.memory_axis.size())
        return std::unexpected{error("invariant_failed", "matrix")};
    auto worker_axis = Json::array();
    JsonCleanup worker_axis_cleanup{worker_axis};
    for (const auto& node : matrix.worker_axis)
        append_json(worker_axis, node_json(node));
    auto memory_axis = Json::array();
    JsonCleanup memory_axis_cleanup{memory_axis};
    for (const auto& node : matrix.memory_axis)
        append_json(memory_axis, node_json(node));
    auto rows = Json::array();
    JsonCleanup rows_cleanup{rows};
    for (std::size_t row_index = 0; row_index < matrix.worker_rows.size(); ++row_index) {
        const auto& row = matrix.worker_rows[row_index];
        if (row.stable_node_id != matrix.worker_axis[row_index].stable_id ||
            row.linux_node != matrix.worker_axis[row_index].linux_node)
            return std::unexpected{error("invariant_failed", "matrix")};
        if (!validate_workers(row.workers, row.linux_cpus))
            return std::unexpected{error("invariant_failed", "matrix")};
        auto workers = Json::array();
        JsonCleanup workers_cleanup{workers};
        for (const auto& worker : row.workers)
            append_json(workers, worker_json(worker));
        append_object_json(rows, [&](Json& json_target_18) {
            set_json(json_target_18, "worker_node", row.stable_node_id.value());
            set_json(json_target_18, "linux_node", row.linux_node.value());
            set_json(json_target_18, "workers", std::move(workers));
        });
    }
    auto cells = Json::array();
    JsonCleanup cells_cleanup{cells};
    std::set<MatrixCellId> ids;
    for (std::size_t cell_index = 0; cell_index < matrix.cells.size(); ++cell_index) {
        const auto& cell = matrix.cells[cell_index];
        const auto expected_row = cell_index / matrix.memory_axis.size();
        const auto expected_column = cell_index % matrix.memory_axis.size();
        if (cell.experiment_index != index || cell.worker_row_index >= matrix.worker_rows.size() ||
            cell.worker_row_index != expected_row ||
            cell.id.experiment_id != matrix.experiment_id ||
            cell.id.worker_node != matrix.worker_rows[cell.worker_row_index].stable_node_id ||
            cell.id.memory_node != matrix.memory_axis[expected_column].stable_id ||
            cell.memory_node_stable_id != matrix.memory_axis[expected_column].stable_id ||
            cell.memory_node != matrix.memory_axis[expected_column].linux_node ||
            !ids.insert(cell.id).second || cell.distance.value == 0U ||
            !validate_nodes(cell.memory_placement))
            return std::unexpected{error("invariant_failed", "matrix")};
        append_object_json(cells, [&](Json& json_target_19) {
            set_json(json_target_19, "id", matrix_id_json(cell.id));
            set_json(json_target_19, "worker_row", cell.id.worker_node.value());
            set_json(json_target_19, "memory_node",
                     node_json({cell.memory_node_stable_id, cell.memory_node}));
            set_json(json_target_19, "memory", placement_json(cell.memory_placement));
            set_json(json_target_19, "distance", json_object([&](Json& json_target_20) {
                         set_json(json_target_20, "meaning", "latency");
                         set_json(json_target_20, "unit", "relative");
                         set_json(json_target_20, "value", cell.distance.value);
                     }));
        });
    }
    auto edges = Json::array();
    JsonCleanup edges_cleanup{edges};
    for (const auto& edge : matrix.comparison_edges) {
        if (edge.baseline_cell_index >= matrix.cells.size() ||
            edge.candidate_cell_index >= matrix.cells.size())
            return std::unexpected{error("invariant_failed", "matrix")};
        append_object_json(edges, [&](Json& json_target_21) {
            set_json(json_target_21, "baseline",
                     matrix_id_json(matrix.cells[edge.baseline_cell_index].id));
            set_json(json_target_21, "candidate",
                     matrix_id_json(matrix.cells[edge.candidate_cell_index].id));
        });
    }
    return json_object([&](Json& json_target_22) {
        set_json(json_target_22, "experiment_index", index);
        set_json(json_target_22, "experiment_id", matrix.experiment_id);
        set_json(json_target_22, "type", "node-pair-matrix");
        set_json(json_target_22, "workload_id", binding.workload_id);
        set_json(json_target_22, "pointer_cycle_seed", seed_json(binding));
        set_json(json_target_22, "worker_axis", std::move(worker_axis));
        set_json(json_target_22, "memory_axis", std::move(memory_axis));
        set_json(json_target_22, "worker_rows", std::move(rows));
        set_json(json_target_22, "cells", std::move(cells));
        set_json(
            json_target_22, "comparison", json_object([&](Json& json_target_23) {
                set_json(json_target_23, "practical_effect",
                         ratio_json(matrix.comparison.practical_effect));
                set_json(json_target_23, "confidence", ratio_json(matrix.comparison.confidence));
                set_json(json_target_23, "order", comparison_order_token(matrix.comparison.order));
                set_json(json_target_23, "order_seed",
                         matrix.comparison.order_seed ? Json{decimal(*matrix.comparison.order_seed)}
                                                      : Json{nullptr});
            }));
        set_json(json_target_22, "comparison_edges", std::move(edges));
    });
}

} // namespace

auto ScenarioDigest::parse(std::string_view text) -> std::expected<ScenarioDigest, core::Error> {
    constexpr std::string_view prefix{"sha256:"};
    if (!text.starts_with(prefix))
        return std::unexpected{error("invariant_failed", "parse_scenario_digest")};
    auto digest = core::Sha256Digest::parse(text.substr(prefix.size()));
    if (!digest)
        return std::unexpected{std::move(digest.error())};
    return ScenarioDigest{std::move(*digest)};
}

auto ScenarioDigest::text() const -> std::string { return "sha256:" + digest_.to_hex(); }
auto ScenarioDigest::bytes() const noexcept -> std::span<const std::byte> {
    return digest_.bytes();
}

auto ResolvedScenarioPlanDigest::parse(std::string_view text)
    -> std::expected<ResolvedScenarioPlanDigest, core::Error> {
    constexpr std::string_view prefix{"sha256:"};
    if (!text.starts_with(prefix))
        return std::unexpected{error("invariant_failed", "parse_resolved_plan_digest")};
    auto digest = core::Sha256Digest::parse(text.substr(prefix.size()));
    if (!digest)
        return std::unexpected{std::move(digest.error())};
    return ResolvedScenarioPlanDigest{std::move(*digest)};
}

auto ResolvedScenarioPlanDigest::text() const -> std::string {
    return "sha256:" + digest_.to_hex();
}
auto ResolvedScenarioPlanDigest::bytes() const noexcept -> std::span<const std::byte> {
    return digest_.bytes();
}

auto detail::projection_implementation::scenario_json(const detail::RetainedPlanProjectionView& scenario)
    -> std::expected<Json, core::Error> {
    try {
        if (!validate_profiles(scenario))
            return std::unexpected{error("prior_stage_profile_mismatch", "scenario_projection")};
        const auto& source = scenario.effective;
        Json workloads = Json::object();
        JsonCleanup workloads_cleanup{workloads};
        for (const auto& [id, workload] : scenario.workloads) {
            auto projected = workload_json(workload);
            JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
            if (!projected)
                return std::unexpected{std::move(projected.error())};
            set_json(workloads, id, std::move(*projected));
        }
        auto experiments = Json::array();
        JsonCleanup experiments_cleanup{experiments};
        for (const auto& experiment : source.at("experiments"))
            append_json(experiments, scenario_experiment_json(experiment));
        const Json empty = Json::object();
        Json worker_resources = Json::object();
        JsonCleanup worker_resources_cleanup{worker_resources};
        Json service_intent = Json::object();
        JsonCleanup service_intent_cleanup{service_intent};
        if (source.contains("resources")) {
            if (source.at("resources").contains("worker_sets"))
                set_json(worker_resources, "worker_sets",
                         semantic_intent(source.at("resources").at("worker_sets")));
            if (source.at("resources").contains("service_cpus"))
                set_json(service_intent, "service_cpus",
                         semantic_intent(source.at("resources").at("service_cpus")));
        }
        return json_object([&](Json& json_target_24) {
            set_json(json_target_24, "profile", scenario_digest_profile);
            set_json(json_target_24, "schema", source.at("schema"));
            set_json(json_target_24, "scenario_id", source.at("metadata").at("id"));
            set_json(json_target_24, "random_seed",
                     decimal(source.at("random").at("seed").get<std::uint64_t>()));
            set_json(json_target_24, "resources", std::move(worker_resources));
            set_json(json_target_24, "service", std::move(service_intent));
            set_json(json_target_24, "workloads", std::move(workloads));
            set_json(json_target_24, "experiments", std::move(experiments));
            set_json(json_target_24, "measurement",
                     semantic_intent(
                         (source.contains("measurement") ? source.at("measurement") : empty)));
            set_json(
                json_target_24, "monitoring",
                semantic_intent((source.contains("monitoring") ? source.at("monitoring") : empty)));
            set_json(json_target_24, "requirements",
                     semantic_intent(
                         (source.contains("requirements") ? source.at("requirements") : empty)));
        });
    } catch (const std::exception&) {
        return std::unexpected{error("scenario_projection_failed", "scenario_projection")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure", "scenario_projection")};
    }
}

auto detail::projection_implementation::resolved_json(const detail::RetainedPlanProjectionView& scenario,
                                          const ScenarioDigest& digest)
    -> std::expected<Json, core::Error> {
    try {
        if (!validate_profiles(scenario))
            return std::unexpected{
                error("prior_stage_profile_mismatch", "resolved_plan_projection")};
        if (!validate_bindings(scenario))
            return std::unexpected{error("invariant_failed", "experiment_bindings")};
        auto service = service_json(scenario);
        JsonPointerCleanup service_cleanup{service ? &*service : nullptr};
        if (!service)
            return std::unexpected{std::move(service.error())};
        Json workloads = Json::object();
        JsonCleanup workloads_cleanup{workloads};
        for (const auto& [id, workload] : scenario.workloads) {
            auto projected = workload_json(workload);
            JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
            if (!projected)
                return std::unexpected{std::move(projected.error())};
            set_json(workloads, id, std::move(*projected));
        }
        const auto& intents = scenario.effective.at("experiments");
        auto experiments = Json::array();
        JsonCleanup experiments_cleanup{experiments};
        std::set<std::size_t> seen_bindings;
        for (std::size_t index = 0; index < intents.size(); ++index) {
            const auto* binding = binding_for(scenario, index);
            if (binding == nullptr || !seen_bindings.insert(index).second)
                return std::unexpected{error("invariant_failed", "experiment_bindings")};
            if (core::detail::json_string_equal(intents.at(index).at("type"), "comparison")) {
                auto projected =
                    comparison_experiment_json(scenario, intents.at(index), index, *binding);
                JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
                if (!projected)
                    return std::unexpected{std::move(projected.error())};
                experiments.push_back(std::move(*projected));
            } else {
                const auto found =
                    std::ranges::find_if(scenario.matrix_experiments, [index](const auto& item) {
                        return item.experiment_index == index;
                    });
                if (found == scenario.matrix_experiments.end())
                    return std::unexpected{error("invariant_failed", "matrix")};
                auto projected =
                    matrix_experiment_json(scenario, intents.at(index), index, *binding, *found);
                JsonPointerCleanup projected_cleanup{projected ? &*projected : nullptr};
                if (!projected)
                    return std::unexpected{std::move(projected.error())};
                experiments.push_back(std::move(*projected));
            }
        }
        const auto& fingerprints = scenario.topology_fingerprints;
        return json_object([&](Json& json_target_25) {
            set_json(json_target_25, "profile", resolved_scenario_plan_digest_profile);
            set_json(json_target_25, "scenario_digest", digest.text());
            set_json(json_target_25, "topology", json_object([&](Json& json_target_26) {
                         set_json(json_target_26, "physical_fingerprint",
                                  fingerprints.physical.to_string());
                         set_json(json_target_26, "effective_fingerprint",
                                  fingerprints.effective.to_string());
                     }));
            set_json(json_target_25, "profiles", json_object([&](Json& json_target_27) {
                         set_json(json_target_27, "resource_resolution",
                                  resource_resolution_profile);
                         set_json(json_target_27, "service_cpu_resolution",
                                  service_cpu_resolution_profile);
                         set_json(json_target_27, "memory_placement_resolution",
                                  memory_placement_resolution_profile);
                         set_json(json_target_27, "kernel_dispatch", kernel_dispatch_profile);
                         set_json(json_target_27, "matrix_expansion", matrix_expansion_profile);
                         set_json(json_target_27, "matrix_cell_identity", matrix_cell_profile);
                     }));
            set_json(json_target_25, "workloads", std::move(workloads));
            set_json(json_target_25, "service", std::move(*service));
            set_json(json_target_25, "experiments", std::move(experiments));
        });
    } catch (const std::exception&) {
        return std::unexpected{
            error("resolved_plan_projection_failed", "resolved_plan_projection")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure", "resolved_plan_projection")};
    }
}

auto scenario_semantic_json(const MatrixExpandedScenario& scenario)
    -> std::expected<Json, core::Error> {
    return detail::projection_implementation::scenario_json(
        detail::retained_plan_projection_view(scenario));
}

auto resolved_scenario_plan_semantic_json(const MatrixExpandedScenario& scenario,
                                         const ScenarioDigest& digest)
    -> std::expected<Json, core::Error> {
    return detail::projection_implementation::resolved_json(
        detail::retained_plan_projection_view(scenario), digest);
}

ScenarioDigestedPlan::ScenarioDigestedPlan(MatrixExpandedScenario scenario,
                                           ScenarioDigest scenario_digest,
                                           ResolvedScenarioPlanDigest resolved_digest,
                                           std::string canonical_scenario,
                                           std::string canonical_resolved_plan)
    : scenario_{std::move(scenario)}, scenario_digest_{std::move(scenario_digest)},
      resolved_digest_{std::move(resolved_digest)},
      canonical_scenario_{std::move(canonical_scenario)},
      canonical_resolved_plan_{std::move(canonical_resolved_plan)} {}

auto ScenarioDigestedPlan::matrix_expanded() const noexcept -> const MatrixExpandedScenario& {
    return scenario_;
}
auto ScenarioDigestedPlan::scenario_digest() const noexcept -> const ScenarioDigest& {
    return scenario_digest_;
}
auto ScenarioDigestedPlan::resolved_scenario_plan_digest() const noexcept
    -> const ResolvedScenarioPlanDigest& {
    return resolved_digest_;
}
auto ScenarioDigestedPlan::canonical_scenario_json() const noexcept -> std::string_view {
    return canonical_scenario_;
}
auto ScenarioDigestedPlan::canonical_resolved_scenario_plan_json() const noexcept
    -> std::string_view {
    return canonical_resolved_plan_;
}

auto compute_scenario_plan_digests(MatrixExpandedScenario scenario)
    -> std::expected<ScenarioDigestedPlan, core::Error> {
    try {
        auto scenario_json = scenario_semantic_json(scenario);
        JsonPointerCleanup scenario_json_cleanup{scenario_json ? &*scenario_json : nullptr};
        if (!scenario_json)
            return std::unexpected{std::move(scenario_json.error())};
        auto scenario_canonical_tree = core::detail::clone_json_as<nlohmann::json>(*scenario_json);
        JsonCleanup scenario_canonical_cleanup{scenario_canonical_tree};
        auto scenario_bytes = core::canonicalize_json(scenario_canonical_tree);
        if (!scenario_bytes)
            return std::unexpected{
                error("canonicalization_failed", "scenario", scenario_bytes.error().code.value())};
        auto scenario_hash = core::hash_canonical_json(scenario_canonical_tree);
        if (!scenario_hash)
            return std::unexpected{
                error("hash_failed", "scenario", scenario_hash.error().code.value())};
        ScenarioDigest scenario_digest{std::move(*scenario_hash)};
        auto plan_json = resolved_scenario_plan_semantic_json(scenario, scenario_digest);
        JsonPointerCleanup plan_json_cleanup{plan_json ? &*plan_json : nullptr};
        if (!plan_json)
            return std::unexpected{std::move(plan_json.error())};
        auto plan_canonical_tree = core::detail::clone_json_as<nlohmann::json>(*plan_json);
        JsonCleanup plan_canonical_cleanup{plan_canonical_tree};
        auto plan_bytes = core::canonicalize_json(plan_canonical_tree);
        if (!plan_bytes)
            return std::unexpected{
                error("canonicalization_failed", "resolved_plan", plan_bytes.error().code.value())};
        auto plan_hash = core::hash_canonical_json(plan_canonical_tree);
        if (!plan_hash)
            return std::unexpected{
                error("hash_failed", "resolved_plan", plan_hash.error().code.value())};
        return ScenarioDigestedPlan{std::move(scenario), std::move(scenario_digest),
                                    ResolvedScenarioPlanDigest{std::move(*plan_hash)},
                                    std::move(*scenario_bytes), std::move(*plan_bytes)};
    } catch (const std::exception&) {
        return std::unexpected{error("contained_exception")};
    } catch (...) {
        return std::unexpected{error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
