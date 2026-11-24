#pragma once

#include "epycbench/topology/stable_id.hpp"

namespace epycbench::topology {

struct UnifiedCacheWitness {
    std::uint32_t level;
    StableCacheId id;
    auto operator==(const UnifiedCacheWitness&) const -> bool = default;
};

struct LocalMemoryNode {
    core::NumaNodeId id;
    StableNumaNodeId stable_id;
    core::LinuxNumaNodeId linux_id;
};

struct L1DataGeometry {
    std::string_view cache_id, worker_id;
    std::uint64_t capacity_bytes, line_bytes, ways;
};

[[nodiscard]] auto unique_l1_data_geometry(const TopologySnapshot&, const StableTopologyIds&,
    const StableProcessingUnitId&) -> std::optional<L1DataGeometry>;

[[nodiscard]] auto highest_unique_unified_cache(const TopologySnapshot&, const StableTopologyIds&,
    core::ProcessingUnitId) -> std::optional<UnifiedCacheWitness>;

[[nodiscard]] auto unique_local_memory_node(const TopologySnapshot&, const StableTopologyIds&,
    core::ProcessingUnitId, core::ByteCount page_size) -> std::optional<LocalMemoryNode>;

} // namespace epycbench::topology
