#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id_set.hpp"
#include "epycbench/core/quantity.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace epycbench::topology {

enum class SourceAvailability { available, unavailable, unknown };
enum class SourceConfidence { reported, derived, inferred, unknown };
enum class TopologyEvidenceSource { hwloc, linux_sysfs, linux_procfs };

struct SourceProvenance {
    TopologyEvidenceSource source{TopologyEvidenceSource::hwloc};
    std::string locator;
    SourceConfidence confidence{SourceConfidence::unknown};
    auto operator<=>(const SourceProvenance&) const = default;
};

template <typename T> struct SourceObservation {
    SourceAvailability availability{SourceAvailability::unknown};
    std::optional<T> value;
    std::vector<SourceProvenance> provenance;
    auto operator==(const SourceObservation&) const -> bool = default;
};

enum class SourceCacheKind { data, instruction, unified, unknown };

struct SourceObjectKey {
    core::CpuSet member_cpus;
    std::optional<std::uint32_t> os_id;
    auto operator==(const SourceObjectKey&) const -> bool = default;
};

// These observations deliberately preserve source semantics. Linux possible, present, and
// online sets describe different physical-resource concepts. Process-allowed sets describe
// process restrictions, cgroup-effective sets describe the resolved cpuset cgroup, and final
// effective sets are derived intersections rather than aliases for either input. A missing
// restriction is unavailable or unknown, never an available empty set. hwloc topology,
// complete, and source-reported allowed sets retain their hwloc meanings. This contract does
// not perform reconciliation, choose source precedence, or project a canonical topology.
struct CpuResourceSetEvidence {
    SourceObservation<core::CpuSet> topology;
    SourceObservation<core::CpuSet> complete;
    SourceObservation<core::CpuSet> source_reported_allowed;
    SourceObservation<core::CpuSet> possible;
    SourceObservation<core::CpuSet> present;
    SourceObservation<core::CpuSet> online;
    SourceObservation<core::CpuSet> offline;
    SourceObservation<core::CpuSet> isolated;
    SourceObservation<core::CpuSet> nohz_full;
    SourceObservation<core::CpuSet> process_allowed;
    SourceObservation<core::CpuSet> cgroup_effective;
    SourceObservation<core::CpuSet> final_effective;
    auto operator==(const CpuResourceSetEvidence&) const -> bool = default;
};

struct NodeResourceSetEvidence {
    SourceObservation<core::NodeSet> topology;
    SourceObservation<core::NodeSet> complete;
    SourceObservation<core::NodeSet> source_reported_allowed;
    SourceObservation<core::NodeSet> possible;
    SourceObservation<core::NodeSet> online;
    SourceObservation<core::NodeSet> has_cpu;
    SourceObservation<core::NodeSet> has_memory;
    SourceObservation<core::NodeSet> process_allowed;
    SourceObservation<core::NodeSet> cgroup_effective;
    SourceObservation<core::NodeSet> final_effective;
    auto operator==(const NodeResourceSetEvidence&) const -> bool = default;
};

struct MachineSourceEvidence {
    SourceObservation<std::string> architecture;
    SourceObservation<std::string> cpu_vendor;
    SourceObservation<std::string> cpu_model;
    SourceObservation<core::ByteCount> total_memory;
    CpuResourceSetEvidence cpu_sets;
    NodeResourceSetEvidence node_sets;
    auto operator==(const MachineSourceEvidence&) const -> bool = default;
};

struct ProcessingUnitSourceEvidence {
    core::LinuxCpuId linux_cpu_id;
    SourceObservation<bool> online;
    SourceObjectKey core;
    std::optional<SourceObjectKey> package;
    std::optional<SourceObjectKey> die;
    std::vector<SourceObjectKey> groups;
    std::vector<SourceProvenance> provenance;
    auto operator==(const ProcessingUnitSourceEvidence&) const -> bool = default;
};

struct PackageSourceEvidence {
    SourceObjectKey key;
    std::vector<SourceProvenance> provenance;
    auto operator==(const PackageSourceEvidence&) const -> bool = default;
};

struct DieSourceEvidence {
    SourceObjectKey key;
    SourceObjectKey package;
    std::vector<SourceProvenance> provenance;
    auto operator==(const DieSourceEvidence&) const -> bool = default;
};

struct ArchitectureGroupSourceEvidence {
    SourceObjectKey key;
    std::string kind;
    SourceObservation<std::string> subtype;
    std::optional<SourceObjectKey> parent;
    std::vector<SourceProvenance> provenance;
    auto operator==(const ArchitectureGroupSourceEvidence&) const -> bool = default;
};

struct CoreSourceEvidence {
    SourceObjectKey key;
    std::optional<SourceObjectKey> package;
    std::optional<SourceObjectKey> die;
    std::vector<SourceProvenance> provenance;
    auto operator==(const CoreSourceEvidence&) const -> bool = default;
};

struct CacheSourceEvidence {
    std::uint32_t level{};
    SourceCacheKind kind{SourceCacheKind::unknown};
    core::ByteCount size{core::ByteCount::from_base_units(0)};
    core::ByteCount line_size{core::ByteCount::from_base_units(0)};
    SourceObservation<std::uint32_t> associativity;
    SourceObservation<std::uint32_t> source_cache_id;
    SourceObservation<std::uint64_t> number_of_sets;
    SourceObservation<std::uint32_t> physical_line_partition;
    SourceObservation<std::string> allocation_policy;
    SourceObservation<std::string> write_policy;
    core::CpuSet shared_cpus;
    std::vector<SourceProvenance> provenance;
    auto operator==(const CacheSourceEvidence&) const -> bool = default;
};

struct NumaNodeSourceEvidence {
    SourceObservation<std::uint32_t> linux_node_id;
    core::CpuSet member_cpus;
    SourceObservation<core::ByteCount> local_memory;
    std::vector<core::ByteCount> page_sizes;
    std::vector<SourceProvenance> provenance;
    auto operator==(const NumaNodeSourceEvidence&) const -> bool = default;
};

enum class DistanceMeaning { latency, bandwidth, unknown };

struct NumaDistanceValue {
    std::uint32_t from_linux_node{};
    std::uint32_t to_linux_node{};
    std::uint64_t value{};
    auto operator<=>(const NumaDistanceValue&) const = default;
};

struct NumaDistanceSourceEvidence {
    std::string name;
    DistanceMeaning meaning{DistanceMeaning::unknown};
    std::string units;
    std::vector<std::uint32_t> linux_nodes;
    std::vector<NumaDistanceValue> directed_values;
    std::vector<SourceProvenance> provenance;
    auto operator==(const NumaDistanceSourceEvidence&) const -> bool = default;
};

struct PciFunctionSourceEvidence {
    std::string bdf;
    std::uint16_t vendor_id{};
    std::uint16_t device_id{};
    std::uint32_t class_code{};
    SourceObservation<std::uint16_t> subsystem_vendor_id;
    SourceObservation<std::uint16_t> subsystem_device_id;
    SourceObservation<core::CpuSet> local_cpus;
    SourceObservation<std::uint32_t> linux_numa_node;
    std::vector<SourceProvenance> provenance;
    auto operator==(const PciFunctionSourceEvidence&) const -> bool = default;
};

struct TopologySourceEvidence {
    TopologyEvidenceSource source{TopologyEvidenceSource::hwloc};
    std::string source_version;
    MachineSourceEvidence machine;
    std::vector<ProcessingUnitSourceEvidence> processing_units;
    std::vector<PackageSourceEvidence> packages;
    std::vector<DieSourceEvidence> dies;
    std::vector<ArchitectureGroupSourceEvidence> architecture_groups;
    std::vector<CoreSourceEvidence> cores;
    std::vector<CacheSourceEvidence> caches;
    std::vector<NumaNodeSourceEvidence> numa_nodes;
    std::vector<NumaDistanceSourceEvidence> numa_distances;
    std::vector<PciFunctionSourceEvidence> pci_functions;
    std::vector<core::Error> diagnostics;
    auto operator==(const TopologySourceEvidence&) const -> bool = default;
};

} // namespace epycbench::topology
