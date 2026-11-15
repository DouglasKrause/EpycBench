#include "scenario/retained_plan_projection.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <nlohmann/json.hpp>
#include <string_view>

namespace epycbench::scenario::detail {
namespace {
using Json = nlohmann::ordered_json;
using Failure = ProjectionInputFailure;

auto utf8(std::string_view value) noexcept -> bool {
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i++]);
        if (lead < 0x80U)
            continue;
        unsigned length{};
        std::uint32_t code{};
        std::uint32_t minimum{};
        if (lead >= 0xc2U && lead <= 0xdfU) {
            length = 1U; code = lead & 0x1fU; minimum = 0x80U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            length = 2U; code = lead & 0x0fU; minimum = 0x800U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            length = 3U; code = lead & 0x07U; minimum = 0x10000U;
        } else {
            return false;
        }
        if (length > value.size() - i)
            return false;
        for (unsigned j = 0; j < length; ++j) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            code = (code << 6U) | (next & 0x3fU);
        }
        if (code < minimum || code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU))
            return false;
    }
    return true;
}
auto identifier(std::string_view text) noexcept -> bool {
    return !text.empty() && text.size() <= 64U && text.front() >= 'a' && text.front() <= 'z' &&
           std::ranges::all_of(text, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}
auto member(const Json& object, std::string_view key) noexcept -> const Json* {
    if (!object.is_object())
        return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}
auto text(const Json* value) noexcept -> std::string_view {
    return value != nullptr && value->is_string()
               ? std::string_view{value->get_ref<const Json::string_t&>()} : std::string_view{};
}
auto unsigned_value(const Json* value) noexcept -> bool {
    return value != nullptr && value->is_number_unsigned();
}
auto ratio(const Json* value) noexcept -> bool {
    if (value == nullptr || !value->is_object() || value->size() != 3U ||
        text(member(*value, "unit")) != "ratio" ||
        !unsigned_value(member(*value, "numerator")) ||
        !unsigned_value(member(*value, "denominator")))
        return false;
    const auto n = member(*value, "numerator")->get<std::uint64_t>();
    const auto d = member(*value, "denominator")->get<std::uint64_t>();
    return d != 0U && n <= d && std::gcd(n, d) == 1U;
}
auto semantic(const Json& value) noexcept -> bool {
    if (value.is_string())
        return utf8(text(&value));
    if (value.is_array())
        return std::ranges::all_of(value, [](const auto& item) { return semantic(item); });
    if (!value.is_object())
        return value.is_null() || value.is_boolean() || value.is_number_integer();
    if (const auto* unit = member(value, "unit")) {
        if (!unit->is_string())
            return false;
        if (text(unit) == "ratio")
            return ratio(&value);
        if (text(unit) == "nanosecond")
            return value.size() == 2U && unsigned_value(member(value, "value"));
    }
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!utf8(it.key()) || !semantic(it.value()))
            return false;
    return true;
}
auto comparison(const Json* value, bool matrix) noexcept -> bool {
    if (value == nullptr || !value->is_object() ||
        !ratio(member(*value, "practical_effect")) || !ratio(member(*value, "confidence")))
        return false;
    const auto baseline = text(member(*value, "baseline"));
    if (!identifier(baseline) || (matrix && baseline != "local-memory"))
        return false;
    const auto* order = member(*value, "order");
    if (order == nullptr || !order->is_object())
        return false;
    const auto strategy = text(member(*order, "strategy"));
    if (strategy != "randomized-abba" && strategy != "randomized-baab" &&
        strategy != "randomized-balanced")
        return false;
    if (const auto* seed = member(*order, "seed"); seed != nullptr && !seed->is_number_unsigned())
        return false;
    if (matrix)
        return true;
    const auto* candidates = member(*value, "candidates");
    if (candidates == nullptr || !candidates->is_array() || candidates->empty())
        return false;
    for (std::size_t i = 0; i < candidates->size(); ++i) {
        if (!identifier(text(&(*candidates)[i])))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if ((*candidates)[j] == (*candidates)[i])
                return false;
    }
    return true;
}
auto effective_domain(const Json& value) noexcept -> bool {
    if (!value.is_object() || text(member(value, "schema")) != "epycbench.scenario/v1")
        return false;
    const auto* metadata = member(value, "metadata");
    const auto* random = member(value, "random");
    const auto* experiments = member(value, "experiments");
    if (metadata == nullptr || !identifier(text(member(*metadata, "id"))) ||
        random == nullptr || !unsigned_value(member(*random, "seed")) ||
        experiments == nullptr || !experiments->is_array() || experiments->empty())
        return false;
    if (const auto* resources = member(value, "resources")) {
        if (!resources->is_object())
            return false;
        for (const auto name : {"worker_sets", "service_cpus"}) {
            if (const auto* sets = member(*resources, name)) {
                if (!sets->is_object() || !semantic(*sets))
                    return false;
                for (auto it = sets->begin(); it != sets->end(); ++it)
                    if (!identifier(it.key()))
                        return false;
            }
        }
    }
    for (const auto name : {"measurement", "monitoring", "requirements"})
        if (const auto* item = member(value, name); item != nullptr &&
            (!item->is_object() || !semantic(*item)))
            return false;
    for (std::size_t i = 0; i < experiments->size(); ++i) {
        const auto& item = (*experiments)[i];
        if (!item.is_object() || !identifier(text(member(item, "id"))) ||
            !identifier(text(member(item, "workload"))) || !semantic(item))
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (text(member((*experiments)[j], "id")) == text(member(item, "id")))
                return false;
        const auto type = text(member(item, "type"));
        if (type == "comparison") {
            const auto* workers = member(item, "workers");
            const auto* variants = member(item, "variants");
            if (workers == nullptr || !identifier(text(workers)) ||
                variants == nullptr || !variants->is_array() || variants->size() < 2U ||
                !comparison(member(item, "compare"), false))
                return false;
            for (std::size_t k = 0; k < variants->size(); ++k) {
                const auto& variant = (*variants)[k];
                const auto* memory = member(variant, "memory");
                if (!identifier(text(member(variant, "id"))) || memory == nullptr ||
                    !memory->is_object())
                    return false;
                for (std::size_t j = 0; j < k; ++j)
                    if (text(member((*variants)[j], "id")) == text(member(variant, "id")))
                        return false;
            }
        } else if (type == "node-pair-matrix") {
            for (const auto key : {"worker_nodes", "memory_nodes"}) {
                const auto* axis = member(item, key);
                if (axis == nullptr || !axis->is_object() || axis->size() != 1U ||
                    text(member(*axis, "select")) !=
                        (std::string_view{key} == "worker_nodes" ? "all-cpu-nodes" : "all-memory-nodes"))
                    return false;
            }
            const auto* workers = member(item, "workers_per_node");
            const auto* select = workers == nullptr ? nullptr : member(*workers, "select");
            const auto* memory = member(item, "memory");
            if (select == nullptr || !select->is_object() ||
                (memory != nullptr && !memory->is_object()) ||
                !comparison(member(item, "comparison"), true))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

struct Inspector {
    ProjectionInputLimits limits;
    ProjectionInputUsage usage{};
    Failure failure{Failure::malformed_input};
    auto add(std::size_t& stock, std::size_t count, std::size_t ceiling) noexcept -> bool {
        if (count > ceiling - stock) {
            failure = Failure::capacity;
            return false;
        }
        stock += count;
        return true;
    }
    auto entries(std::size_t count) noexcept -> bool {
        return add(usage.typed_entries, count, limits.typed_entries);
    }
    auto string(std::string_view value, bool projected = true) noexcept -> bool {
        return add(usage.string_bytes, value.size(), limits.string_bytes) &&
               (!projected || utf8(value));
    }
    auto id(std::string_view value) noexcept -> bool { return string(value) && identifier(value); }
    template<class T> auto stable(const T& value) noexcept -> bool {
        return !value.value().empty() && string(value.value());
    }
    auto cpus(const core::CpuSet& value) noexcept -> bool {
        const auto count = value.cardinality();
        if (count > limits.cpu_cardinality - usage.cpu_cardinality) {
            failure = Failure::capacity;
            return false;
        }
        usage.cpu_cardinality += static_cast<std::size_t>(count);
        return true;
    }
    auto tree(const Json& value, std::size_t depth) noexcept -> bool {
        if (depth > limits.depth) {
            failure = Failure::capacity;
            return false;
        }
        usage.depth = std::max(usage.depth, depth);
        if (!add(usage.json_nodes, 1U, limits.json_nodes))
            return false;
        if (value.is_string())
            return string(text(&value), false);
        if (!value.is_structured())
            return true;
        if (value.size() > limits.json_nodes - usage.json_nodes) {
            failure = Failure::capacity;
            return false;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (value.is_object() && !string(it.key(), false))
                return false;
            if (!tree(it.value(), depth + 1U))
                return false;
        }
        return true;
    }
    template<class Container, class Function>
    auto each(const Container& values, Function&& check) noexcept -> bool {
        if (!entries(values.size()))
            return false;
        for (const auto& value : values)
            if (!check(value))
                return false;
        return true;
    }
    template<class Worker> auto worker(const Worker& value) noexcept -> bool {
        return stable(value.stable_id) && stable(value.core_id);
    }
    auto node(const ResolvedMemoryNode& value) noexcept -> bool { return stable(value.stable_id); }
    template<class Placement> auto placement_base(const Placement& value) noexcept -> bool {
        return value.policy == MemoryPolicy::bind && value.page_mode == PageMode::base &&
               value.page_size.value() != 0U && string(value.source_pointer, false);
    }
    auto placement(const ResolvedMemoryPlacement& value) noexcept -> bool {
        return placement_base(value) && !value.nodes.empty() &&
               each(value.nodes, [&](const auto& item) { return node(item); }) &&
               entries(value.linux_nodes.size());
    }
    auto placement(const ResolvedPerWorkerMemoryPlacement& value) noexcept -> bool {
        return placement_base(value) && !value.worker_nodes.empty() &&
               each(value.worker_nodes, [&](const auto& item) {
                   return worker(item.worker) && node(item.node);
               });
    }
    auto placement(const ComparisonMemoryPlacement& value) noexcept -> bool {
        return !value.valueless_by_exception() &&
               std::visit([&](const auto& item) { return placement(item); }, value);
    }
    static auto configuration(const MatrixComparisonConfiguration& value) noexcept -> bool {
        switch (value.order) {
        case MatrixComparisonOrder::randomized_abba:
        case MatrixComparisonOrder::randomized_baab:
        case MatrixComparisonOrder::randomized_balanced: return true;
        }
        return false;
    }
    auto workload(const DispatchedWorkload& value) noexcept -> bool {
        if (!id(value.workload_id) || !string(value.kernel_token) ||
            value.kernel_token != kernel::kernel_id_token(value.kernel_id) ||
            !string(value.source_pointer, false))
            return false;
        if (value.kernel_id.family() == kernel::KernelFamily::bandwidth)
            return std::holds_alternative<kernel::SequentialReadBandwidthConfig>(value.configuration);
        if (value.kernel_id.family() != kernel::KernelFamily::latency ||
            !std::holds_alternative<kernel::DependentLoadLatencyConfig>(value.configuration))
            return false;
        const auto& seed = std::get<kernel::DependentLoadLatencyConfig>(value.configuration).pointer_cycle_seed();
        return (seed.mode() == kernel::PointerCycleSeedMode::explicit_value && seed.value()) ||
               (seed.mode() == kernel::PointerCycleSeedMode::derive_per_experiment && !seed.value());
    }
    auto facts(const RetainedPlanProjectionView& view, std::size_t experiments) noexcept -> bool {
        if (!each(view.worker_sets, [&](const auto& item) {
                const auto& value = item.second;
                return id(item.first) && id(value.name) && string(value.source_pointer, false) &&
                       cpus(value.linux_cpus) && each(value.processing_units, [&](const auto& w) { return worker(w); });
            }) || !each(view.experiment_workers, [&](const auto& value) {
                return value.experiment_index < experiments && id(value.worker_set_name) &&
                       string(value.source_pointer, false);
            }) || !each(view.service_cpu_pools, [&](const auto& item) {
                const auto& value = item.second;
                return id(item.first) && id(value.name) && string(value.source_pointer, false) &&
                       cpus(value.linux_cpus) && each(value.processing_units, [&](const auto& w) { return worker(w); }) &&
                       each(value.reserved_cores, [&](const auto& c) { return stable(c); });
            }) || !cpus(view.reserved_service_cpus) ||
            !each(view.reserved_service_cores, [&](const auto& c) { return stable(c); }) ||
            !each(view.comparison_memory, [&](const auto& value) {
                return value.experiment_index < experiments && placement(value.placement);
            }) || !each(view.workloads, [&](const auto& item) { return id(item.first) && workload(item.second); }) ||
            !each(view.experiment_workloads, [&](const auto& value) {
                if (value.experiment_index >= experiments || !id(value.workload_id) ||
                    !string(value.source_pointer, false) ||
                    value.pointer_cycle_seed.has_value() != value.pointer_cycle_seed_origin.has_value())
                    return false;
                if (value.pointer_cycle_seed_origin &&
                    *value.pointer_cycle_seed_origin != PointerCycleSeedOrigin::explicit_value &&
                    *value.pointer_cycle_seed_origin != PointerCycleSeedOrigin::derived_value)
                    return false;
                if (!each(value.deferred_constraints, [](auto constraint) {
                        return constraint == DeferredKernelConstraint::working_set_at_least_resolved_page_size ||
                               constraint == DeferredKernelConstraint::exactly_one_worker;
                    }))
                    return false;
                return value.deferred_constraints.size() <= 2U &&
                       (value.deferred_constraints.size() != 2U ||
                        (value.deferred_constraints[0] == DeferredKernelConstraint::working_set_at_least_resolved_page_size &&
                         value.deferred_constraints[1] == DeferredKernelConstraint::exactly_one_worker));
            }))
            return false;
        if (!each(view.comparison_experiments, [&](const auto& value) {
                const auto origin = value.practical_effect_origin.kind;
                if (origin != ValueOriginKind::explicit_value && origin != ValueOriginKind::schema_default &&
                    origin != ValueOriginKind::scenario_default && origin != ValueOriginKind::experiment_override &&
                    origin != ValueOriginKind::variant_override && origin != ValueOriginKind::matrix_override)
                    return false;
                return value.experiment_index < experiments && id(value.experiment_id) && id(value.workload_id) &&
                       cpus(value.linux_cpus) && configuration(value.comparison) &&
                       each(value.workers, [&](const auto& w) { return worker(w); }) &&
                       each(value.variants, [&](const auto& variant) {
                           return variant.variant_index < value.variants.size() && id(variant.variant_id) && placement(variant.memory_placement);
                       }) && id(value.baseline_variant_id) &&
                       each(value.candidate_variant_ids, [&](const auto& candidate) { return id(candidate); }) &&
                       (!value.practical_effect_origin.source_pointer || string(*value.practical_effect_origin.source_pointer));
            }) || !each(view.matrix_experiments, [&](const auto& value) {
                if (value.experiment_index >= experiments || !id(value.experiment_id) ||
                    !configuration(value.comparison) || value.worker_axis.empty() || value.memory_axis.empty() ||
                    value.worker_axis.size() > projection_input_max / value.memory_axis.size() ||
                    value.cells.size() != value.worker_axis.size() * value.memory_axis.size() ||
                    value.worker_rows.size() != value.worker_axis.size())
                    return false;
                return each(value.worker_axis, [&](const auto& n) { return node(n); }) &&
                       each(value.memory_axis, [&](const auto& n) { return node(n); }) &&
                       each(value.worker_rows, [&](const auto& row) {
                           return stable(row.stable_node_id) && cpus(row.linux_cpus) &&
                                  each(row.workers, [&](const auto& w) { return worker(w); });
                       }) && each(value.cells, [&](const auto& cell) {
                           return cell.experiment_index < experiments && cell.worker_row_index < value.worker_rows.size() &&
                                  cell.distance.value > 0U && id(cell.id.experiment_id) && stable(cell.id.worker_node) &&
                                  stable(cell.id.memory_node) && stable(cell.memory_node_stable_id) && placement(cell.memory_placement);
                       }) && each(value.comparison_edges, [&](const auto& edge) {
                           return edge.baseline_cell_index < value.cells.size() && edge.candidate_cell_index < value.cells.size();
                       });
            }))
            return false;
        return string(view.resource_profile) && string(view.service_profile) && string(view.memory_profile) &&
               string(view.dispatch_profile) && string(view.expansion_profile) &&
               view.resource_profile == resource_resolution_profile && view.service_profile == service_cpu_resolution_profile &&
               view.memory_profile == memory_placement_resolution_profile && view.dispatch_profile == kernel_dispatch_profile &&
               view.expansion_profile == matrix_expansion_profile;
    }
};

template<class Values, class Key>
auto unique(const Values& values, Key&& key) noexcept -> bool {
    for (std::size_t i = 0; i < values.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (key(values[i]) == key(values[j]))
                return false;
    return true;
}
template<class Workers>
auto worker_domain(const Workers& workers, const core::CpuSet& cpus) noexcept -> bool {
    return !workers.empty() && cpus.cardinality() == workers.size() &&
           unique(workers, [](const auto& w) { return w.linux_cpu; }) &&
           unique(workers, [](const auto& w) { return w.stable_id.value(); }) &&
           std::ranges::all_of(workers, [&](const auto& w) { return cpus.contains(w.linux_cpu); });
}
auto placement_domain(const ResolvedMemoryPlacement& value) noexcept -> bool {
    return value.nodes.size() == value.linux_nodes.size() &&
           unique(value.nodes, [](const auto& n) { return n.stable_id.value(); }) &&
           unique(value.nodes, [](const auto& n) { return n.linux_node; }) &&
           std::ranges::all_of(value.nodes, [&](const auto& n) { return value.linux_nodes.contains(n.linux_node); });
}
auto placement_domain(const ResolvedPerWorkerMemoryPlacement& value) noexcept -> bool {
    return unique(value.worker_nodes, [](const auto& n) { return n.worker.linux_cpu; }) &&
           unique(value.worker_nodes, [](const auto& n) { return n.worker.stable_id.value(); });
}
auto placement_domain(const ComparisonMemoryPlacement& value) noexcept -> bool {
    return std::visit([](const auto& p) { return placement_domain(p); }, value);
}
auto typed_domains(const RetainedPlanProjectionView& view) noexcept -> bool {
    const auto& intents = *member(view.effective, "experiments");
    const auto index = [](const auto& item) { return item.experiment_index; };
    if (view.experiment_workloads.size() != intents.size() ||
        view.comparison_experiments.size() > intents.size() ||
        view.matrix_experiments.size() != intents.size() - view.comparison_experiments.size() ||
        view.experiment_workers.size() != view.comparison_experiments.size() ||
        !unique(view.experiment_workloads, index) || !unique(view.experiment_workers, index) ||
        !unique(view.comparison_experiments, index) || !unique(view.matrix_experiments, index))
        return false;
    for (const auto& [name, workers] : view.worker_sets)
        if (name != workers.name || !worker_domain(workers.processing_units, workers.linux_cpus))
            return false;
    for (const auto& [name, pool] : view.service_cpu_pools)
        if (name != pool.name || !worker_domain(pool.processing_units, pool.linux_cpus))
            return false;
    for (const auto& [name, workload] : view.workloads)
        if (name != workload.workload_id)
            return false;
    for (const auto& binding : view.experiment_workloads) {
        const auto found = view.workloads.find(binding.workload_id);
        if (found == view.workloads.end())
            return false;
        const bool latency = found->second.kernel_id.family() == kernel::KernelFamily::latency;
        if (latency != binding.pointer_cycle_seed.has_value())
            return false;
    }
    for (const auto& reference : view.experiment_workers)
        if (text(member(intents[reference.experiment_index], "type")) != "comparison" ||
            !view.worker_sets.contains(reference.worker_set_name))
            return false;
    for (const auto& value : view.comparison_experiments) {
        if (text(member(intents[value.experiment_index], "type")) != "comparison" ||
            !worker_domain(value.workers, value.linux_cpus) || value.variants.size() < 2U ||
            value.candidate_variant_ids.empty() ||
            !unique(value.variants, [](const auto& v) { return std::string_view{v.variant_id}; }) ||
            !unique(value.candidate_variant_ids, [](const auto& id) { return std::string_view{id}; }))
            return false;
        for (std::size_t i = 0; i < value.variants.size(); ++i)
            if (value.variants[i].variant_index != i || !placement_domain(value.variants[i].memory_placement))
                return false;
    }
    for (const auto& value : view.comparison_memory) {
        const auto& intent = intents[value.experiment_index];
        const auto* variants = member(intent, "variants");
        if (text(member(intent, "type")) != "comparison" || variants == nullptr ||
            !variants->is_array() || value.variant_index >= variants->size() || !placement_domain(value.placement))
            return false;
    }
    for (std::size_t i = 0; i < view.comparison_memory.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (view.comparison_memory[i].experiment_index == view.comparison_memory[j].experiment_index &&
                view.comparison_memory[i].variant_index == view.comparison_memory[j].variant_index)
                return false;
    for (const auto& matrix : view.matrix_experiments) {
        if (text(member(intents[matrix.experiment_index], "type")) != "node-pair-matrix" ||
            !unique(matrix.worker_axis, [](const auto& n) { return n.stable_id.value(); }) ||
            !unique(matrix.memory_axis, [](const auto& n) { return n.stable_id.value(); }))
            return false;
        for (const auto& row : matrix.worker_rows)
            if (!worker_domain(row.workers, row.linux_cpus))
                return false;
        for (const auto& cell : matrix.cells)
            if (!placement_domain(cell.memory_placement))
                return false;
    }
    return true;
}

auto rejection(Failure failure) -> core::Error {
    const auto code = failure == Failure::invalid_limits ? "scenario.projection.invalid_limits" :
                      failure == Failure::capacity ? "scenario.projection.input_capacity" :
                                                     "scenario.projection.malformed_input";
    return {.category = core::ErrorCategory::scenario,
            .code = core::StableErrorCode{code},
            .message = "Retained projection input rejected",
            .operation = "inspect_retained_plan_projection",
            .os_error = std::nullopt, .context = nullptr,
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
} // namespace

auto retained_plan_projection_view(const MatrixExpandedScenario& value) noexcept -> RetainedPlanProjectionView {
    const auto& kernel = value.kernel_dispatched();
    const auto& memory = kernel.memory_placement_resolved();
    const auto& service = memory.service_cpu_resolved();
    const auto& resources = service.resource_resolved();
    return {resources.default_expanded().value(), resources.worker_sets(), resources.experiment_workers(),
            service.service_cpu_pools(), service.reserved_service_cpus(), service.reserved_service_cores(),
            service.has_dedicated_service_placement(), memory.comparison_memory(), kernel.workloads(),
            kernel.experiment_workloads(), value.comparison_experiments(), value.matrix_experiments(),
            resources.topology_fingerprints(), resources.profile(), service.resolution_profile(),
            memory.resolution_profile(), kernel.dispatch_profile(), value.expansion_profile()};
}

auto inspect_retained_plan_projection(const RetainedPlanProjectionView& view, ProjectionInputLimits limits) noexcept
    -> std::expected<ProjectionInputUsage, ProjectionInputFailure> {
    if (limits.json_nodes > projection_input_max || limits.typed_entries > projection_input_max ||
        limits.string_bytes > projection_input_max || limits.cpu_cardinality > projection_input_max ||
        limits.pairwise_work > projection_input_max || limits.depth > 64U)
        return std::unexpected{Failure::invalid_limits};
    Inspector inspector{limits};
    if (!inspector.tree(view.effective, 0U))
        return std::unexpected{inspector.failure};
    const auto* experiments = member(view.effective, "experiments");
    if (experiments == nullptr || !experiments->is_array())
        return std::unexpected{Failure::malformed_input};
    if (!inspector.facts(view, experiments->size()))
        return std::unexpected{inspector.failure};
    auto size = inspector.usage.json_nodes;
    if (!inspector.add(size, inspector.usage.typed_entries, projection_input_max) ||
        !inspector.add(size, inspector.usage.cpu_cardinality, projection_input_max) ||
        (size != 0U && size > limits.pairwise_work / size))
        return std::unexpected{Failure::capacity};
    inspector.usage.pairwise_work = size * size;
    if (!effective_domain(view.effective) || !typed_domains(view))
        return std::unexpected{Failure::malformed_input};
    return inspector.usage;
}

auto retained_scenario_semantic_json(const RetainedPlanProjectionView& view, ProjectionInputLimits limits)
    -> std::expected<Json, core::Error> {
    const auto checked = inspect_retained_plan_projection(view, limits);
    if (!checked)
        return std::unexpected{rejection(checked.error())};
    return projection_implementation::scenario_json(view);
}
auto retained_resolved_plan_semantic_json(const RetainedPlanProjectionView& view, const ScenarioDigest& digest,
                                         ProjectionInputLimits limits) -> std::expected<Json, core::Error> {
    const auto checked = inspect_retained_plan_projection(view, limits);
    if (!checked)
        return std::unexpected{rejection(checked.error())};
    return projection_implementation::resolved_json(view, digest);
}
} // namespace epycbench::scenario::detail
