#include "epycbench/topology/worker_locality.hpp"

#include <algorithm>

namespace epycbench::topology {

auto unique_l1_data_geometry(const TopologySnapshot& snapshot, const StableTopologyIds& ids,
    const StableProcessingUnitId& worker) -> std::optional<L1DataGeometry> {
    const auto pu=ids.processing_unit(worker);
    if (!pu) return {};
    const CacheDomain* l1=nullptr;
    for (const auto& cache:snapshot.caches()) {
        if (cache.level!=1U || !cache.member_pus.contains(*pu) ||
            (cache.kind!=CacheKind::data && cache.kind!=CacheKind::unified)) continue;
        if (l1) return {};
        l1=&cache;
    }
    if (!l1 || !l1->associativity.value()) return {};
    auto id=ids.cache_id(l1->id);
    if (!id) return {};
    return L1DataGeometry{id->get().value(),worker.value(),l1->size.value(),l1->line_size.value(),*l1->associativity.value()};
}

auto highest_unique_unified_cache(const TopologySnapshot& snapshot, const StableTopologyIds& ids,
    core::ProcessingUnitId pu) -> std::optional<UnifiedCacheWitness> {
    std::uint32_t level{};
    std::optional<core::CacheId> unique;
    for (const auto id : snapshot.caches_for_pu(pu)) {
        const auto cache = snapshot.cache(id);
        if (!cache || cache->kind != CacheKind::unified) continue;
        if (cache->level > level) {
            level = cache->level;
            unique = id;
        } else if (cache->level == level) {
            unique.reset();
        }
    }
    if (!unique) return {};
    const auto stable = ids.cache_id(*unique);
    if (!stable) return {};
    return UnifiedCacheWitness{level, stable->get()};
}

auto unique_local_memory_node(const TopologySnapshot& snapshot, const StableTopologyIds& ids,
    core::ProcessingUnitId pu, core::ByteCount page_size) -> std::optional<LocalMemoryNode> {
    const auto& machine = snapshot.machine();
    const auto& effective = machine.effective_nodes.value();
    if (!effective) return {};

    std::optional<LocalMemoryNode> local;
    for (const auto id : snapshot.numa_nodes_for_pu(pu)) {
        const auto node = snapshot.numa_node(id);
        if (!node || !node->linux_node_id.value() || !node->installed_memory.value() ||
            node->installed_memory.value()->value() == 0U) continue;
        const auto linux = *node->linux_node_id.value();
        if (!machine.online_nodes.contains(core::NumaNodeId{linux}) ||
            !effective->contains(core::NumaNodeId{linux})) continue;
        const auto stable = ids.numa_node_id(id);
        if (local || !stable || !std::ranges::contains(node->page_sizes, page_size)) return {};
        local.emplace(id, stable->get(), core::LinuxNumaNodeId{linux});
    }
    return local;
}

} // namespace epycbench::topology
