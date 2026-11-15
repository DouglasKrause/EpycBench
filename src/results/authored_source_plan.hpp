#pragma once
#include "results/authored_source.hpp"
#include <array>
#include <limits>
#include <nlohmann/json.hpp>

namespace epycbench::results::authored_source::detail {
struct LimitFailure {};
struct Counter {
    Limits limits;
    Usage used{};
    void add(std::size_t n) {
        if (n > limits.total_bytes - used.bytes)
            throw LimitFailure{};
        used.bytes += n;
    }
    void elements(std::uint64_t count, std::size_t width) {
        if (count > (limits.total_bytes - used.bytes) / width)
            throw LimitFailure{};
        add(static_cast<std::size_t>(count) * width);
    }
    void text(std::string_view s) { add(s.size()); }
    template <class C> void array(const C& c) {
        elements(c.size(), sizeof(typename C::value_type));
    }
    void cpus(const core::CpuSet& c) { elements(c.cardinality(), 2 * sizeof(std::uint32_t)); }
    template <class Id> void id(const Id& v) { text(v.value()); }
    template <class J> void json(const J& root) {
        struct Frame {
            const J* parent;
            typename J::const_iterator next;
        };
        std::array<Frame, 65> stack{};
        std::size_t depth = 0;
        const J* current = &root;
        for (;;) {
            if (used.nodes == limits.nodes)
                throw LimitFailure{};
            ++used.nodes;
            add(sizeof(J));
            if (current->is_string())
                text(current->template get_ref<const typename J::string_t&>());
            if (current->is_structured() && !current->empty()) {
                if (depth == 64)
                    throw LimitFailure{};
                auto next = current->begin();
                if (current->is_object()) {
                    add(sizeof(typename J::string_t));
                    text(next.key());
                }
                const auto* child = &next.value();
                stack[depth++] = {current, ++next};
                current = child;
                continue;
            }
            while (depth != 0 && stack[depth - 1].next == stack[depth - 1].parent->end())
                --depth;
            if (depth == 0)
                break;
            auto& frame = stack[depth - 1];
            if (frame.parent->is_object()) {
                add(sizeof(typename J::string_t));
                text(frame.next.key());
            }
            current = &frame.next.value();
            ++frame.next;
        }
    }
    void pu(const scenario::ResolvedProcessingUnit& p) {
        id(p.stable_id);
        id(p.core_id);
    }
    void node(const scenario::ResolvedMemoryNode& n) { id(n.stable_id); }
    void memory(const scenario::ResolvedMemoryPlacement& m) {
        array(m.nodes);
        for (const auto& n : m.nodes)
            node(n);
        array(m.linux_nodes);
        text(m.source_pointer);
    }
    void memory(const scenario::ResolvedPerWorkerMemoryPlacement& m) {
        array(m.worker_nodes);
        for (const auto& n : m.worker_nodes) {
            pu(n.worker);
            node(n.node);
        }
        text(m.source_pointer);
    }
    void memory(const scenario::ComparisonMemoryPlacement& m) {
        std::visit([&](const auto& v) { memory(v); }, m);
    }
    void origin(const scenario::ValueOrigin& o) {
        if (o.source_pointer)
            text(*o.source_pointer);
    }
};
inline auto authored(const scenario::ScenarioDigestedPlan& p) -> const scenario::AuthoredScenario& {
    return p.matrix_expanded()
        .kernel_dispatched()
        .memory_placement_resolved()
        .service_cpu_resolved()
        .resource_resolved()
        .default_expanded()
        .unit_normalized()
        .structurally_valid()
        .authored();
}
inline void count_plan(Counter& c, const scenario::ScenarioDigestedPlan& p) {
    c.add(sizeof(p));
    const auto canonical = p.canonical_scenario_json();
    const auto resolved = p.canonical_resolved_scenario_plan_json();
    if (canonical.size() > c.limits.canonical_bytes ||
        resolved.size() > c.limits.canonical_bytes - canonical.size())
        throw LimitFailure{};
    c.used.canonical_bytes = canonical.size() + resolved.size();
    c.text(canonical);
    c.text(resolved);
    const auto& matrix = p.matrix_expanded();
    const auto& kernel = matrix.kernel_dispatched();
    const auto& memory = kernel.memory_placement_resolved();
    const auto& service = memory.service_cpu_resolved();
    const auto& resource = service.resource_resolved();
    const auto& defaults = resource.default_expanded();
    const auto& units = defaults.unit_normalized();
    const auto& source = units.structurally_valid().authored();
    c.text(source.source_name());
    c.json(source.value());
    c.json(units.value());
    c.json(defaults.value());
    c.array(source.locations());
    for (const auto& [k, v] : source.locations()) {
        (void)v;
        c.text(k);
    }
    c.array(defaults.origins());
    for (const auto& [k, v] : defaults.origins()) {
        c.text(k);
        c.origin(v);
    }
    c.array(resource.worker_sets());
    for (const auto& [k, v] : resource.worker_sets()) {
        c.text(k);
        c.text(v.name);
        c.text(v.source_pointer);
        c.cpus(v.linux_cpus);
        c.array(v.processing_units);
        for (const auto& x : v.processing_units)
            c.pu(x);
    }
    c.array(resource.experiment_workers());
    for (const auto& v : resource.experiment_workers()) {
        c.text(v.worker_set_name);
        c.text(v.source_pointer);
    }
    c.array(resource.matrix_worker_templates());
    for (const auto& v : resource.matrix_worker_templates()) {
        c.text(v.source_pointer);
        c.text(v.profile);
        c.array(v.smt_indices);
        c.array(v.filters.packages);
        c.array(v.filters.numa_nodes);
        c.array(v.filters.dies);
        c.array(v.filters.cache_domains);
        c.array(v.exclusions.processing_units);
        c.array(v.exclusions.cores);
        c.array(v.exclusions.worker_sets);
        for (const auto& x : v.exclusions.worker_sets)
            c.text(x);
    }
    c.array(service.service_cpu_pools());
    for (const auto& [k, v] : service.service_cpu_pools()) {
        c.text(k);
        c.text(v.name);
        c.text(v.source_pointer);
        c.cpus(v.linux_cpus);
        c.array(v.processing_units);
        for (const auto& x : v.processing_units) {
            c.id(x.stable_id);
            c.id(x.core_id);
        }
        c.array(v.reserved_cores);
        for (const auto& x : v.reserved_cores)
            c.id(x);
    }
    c.cpus(service.reserved_service_cpus());
    c.array(service.reserved_service_cores());
    for (const auto& x : service.reserved_service_cores())
        c.id(x);
    c.array(memory.comparison_memory());
    for (const auto& v : memory.comparison_memory())
        c.memory(v.placement);
    c.array(memory.matrix_memory_templates());
    for (const auto& v : memory.matrix_memory_templates()) {
        c.text(v.source_pointer);
        c.text(v.profile);
    }
    c.array(kernel.workloads());
    for (const auto& [k, v] : kernel.workloads()) {
        c.text(k);
        c.text(v.workload_id);
        c.text(v.kernel_token);
        c.text(v.source_pointer);
    }
    c.array(kernel.experiment_workloads());
    for (const auto& v : kernel.experiment_workloads()) {
        c.text(v.workload_id);
        c.text(v.source_pointer);
        c.array(v.deferred_constraints);
    }
    c.array(matrix.matrix_experiments());
    for (const auto& v : matrix.matrix_experiments()) {
        c.text(v.experiment_id);
        c.array(v.worker_axis);
        for (const auto& x : v.worker_axis)
            c.node(x);
        c.array(v.memory_axis);
        for (const auto& x : v.memory_axis)
            c.node(x);
        c.array(v.worker_rows);
        for (const auto& x : v.worker_rows) {
            c.id(x.stable_node_id);
            c.cpus(x.linux_cpus);
            c.array(x.workers);
            for (const auto& w : x.workers)
                c.pu(w);
        }
        c.array(v.cells);
        for (const auto& x : v.cells) {
            c.text(x.id.experiment_id);
            c.id(x.id.worker_node);
            c.id(x.id.memory_node);
            c.id(x.memory_node_stable_id);
            c.memory(x.memory_placement);
        }
        c.array(v.comparison_edges);
    }
    c.array(matrix.comparison_experiments());
    for (const auto& v : matrix.comparison_experiments()) {
        c.text(v.experiment_id);
        c.text(v.workload_id);
        c.text(v.baseline_variant_id);
        c.array(v.candidate_variant_ids);
        for (const auto& x : v.candidate_variant_ids)
            c.text(x);
        c.cpus(v.linux_cpus);
        c.array(v.workers);
        for (const auto& x : v.workers)
            c.pu(x);
        c.array(v.variants);
        for (const auto& x : v.variants) {
            c.text(x.variant_id);
            c.memory(x.memory_placement);
        }
        c.origin(v.practical_effect_origin);
    }
}
} // namespace epycbench::results::authored_source::detail
