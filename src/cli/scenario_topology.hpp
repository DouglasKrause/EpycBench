#pragma once

#include "epycbench/topology/model.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace epycbench::cli::scenario_topology {
using Workers = std::vector<const topology::ProcessingUnit*>;
using MemoryNodes = std::vector<const topology::NumaNode*>;
inline constexpr std::uint64_t dense_memory_line_bytes = 64U;

inline auto eligible_workers(const topology::TopologySnapshot& topology) -> Workers {
    Workers workers;
    const auto& effective = topology.machine().effective_cpus.value();
    if (!effective) return workers;
    for (const auto& pu : topology.processing_units()) {
        if (effective->contains(pu.linux_cpu_id)) workers.push_back(&pu);
    }
    std::ranges::sort(workers, {}, &topology::ProcessingUnit::linux_cpu_id);
    return workers;
}

inline auto locally_placed_workers(const topology::TopologySnapshot& topology, const Workers& workers,
                            const std::vector<const topology::NumaNode*>& nodes) -> Workers {
    Workers local;
    for (const auto* worker : workers) {
        if (topology.numa_nodes_for_pu(worker->id).size() == 1U &&
            std::ranges::any_of(nodes, [worker](const auto* node) {
                return node->member_pus.contains(worker->id);
            }))
            local.push_back(worker);
    }
    return local;
}

inline auto eligible_nodes(const topology::TopologySnapshot& topology) -> std::vector<const topology::NumaNode*> {
    std::vector<const topology::NumaNode*> nodes;
    const auto& effective = topology.machine().effective_nodes.value();
    if (!effective) return nodes;
    for (const auto& node : topology.numa_nodes()) {
        const auto& id = node.linux_node_id.value();
        const auto& bytes = node.installed_memory.value();
        if (id && bytes && bytes->value() != 0U && effective->contains(core::NumaNodeId{*id}))
            nodes.push_back(&node);
    }
    std::ranges::sort(nodes, {}, [](const auto* node) { return *node->linux_node_id.value(); });
    return nodes;
}

inline auto base_page_nodes(const topology::TopologySnapshot& topology, const MemoryNodes& nodes)
    -> MemoryNodes {
    if (topology.machine().page_sizes.empty()) return {};
    const auto page = std::ranges::min(topology.machine().page_sizes);
    if (page.value() == 0U) return {};
    MemoryNodes eligible;
    for (const auto* node : nodes)
        if (topology.machine().online_nodes.contains(core::NumaNodeId{*node->linux_node_id.value()}) &&
            std::ranges::contains(node->page_sizes, page)) eligible.push_back(node);
    return eligible;
}

struct CacheSweepSite {
    const topology::ProcessingUnit* worker;
    const topology::NumaNode* local;
    MemoryNodes remote;
    std::vector<const topology::CacheDomain*> caches;
    std::set<std::uint64_t> working_sets;
};

inline auto cache_sweep_sites(const topology::TopologySnapshot& topology, const Workers& workers,
                       const MemoryNodes& nodes) -> std::vector<CacheSweepSite> {
    if (nodes.size() < 2U)
        return {};
    const auto page = std::ranges::min(topology.machine().page_sizes).value();
    constexpr auto stride = dense_memory_line_bytes;
    constexpr auto maximum_capacity = (std::numeric_limits<std::uint64_t>::max() - (stride - 1U)) / 2U;
    std::vector<CacheSweepSite> sites;
    for (const auto* worker : locally_placed_workers(topology, workers, nodes)) {
        const auto local = *std::ranges::find_if(nodes, [&](const auto* n) { return n->member_pus.contains(worker->id); });
        CacheSweepSite site{worker, local, {}, {}, {}};
        std::map<std::uint32_t, const topology::CacheDomain*> levels;
        for (const auto& cache : topology.caches()) {
            if (!cache.member_pus.contains(worker->id) ||
                (cache.kind != topology::CacheKind::unified && cache.kind != topology::CacheKind::data)) continue;
            const auto [it, first] = levels.try_emplace(cache.level, &cache);
            if (!first) it->second = nullptr;
        }
        for (const auto& [level, cache] : levels) {
            if (!level || !cache || cache->size.value() > maximum_capacity) continue;
            const auto capacity = cache->size.value();
            const auto below = capacity / 2U / stride * stride;
            const auto above = (2U * capacity + stride - 1U) / stride * stride;
            if (below < std::max<std::uint64_t>(page, 2U * stride)) continue;
            site.working_sets.insert(below);
            site.working_sets.insert(above);
            site.caches.push_back(cache);
        }
        if (site.working_sets.empty()) continue;
        const auto largest = *site.working_sets.rbegin();
        const auto fits = [largest, page](const auto* node) {
            return largest <= node->installed_memory.value()->value() / page * page;
        };
        if (!fits(local)) continue;
        for (const auto* node : nodes)
            if (node != local && fits(node)) site.remote.push_back(node);
        if (!site.remote.empty()) sites.push_back(std::move(site));
    }
    return sites;
}

} // namespace epycbench::cli::scenario_topology
