#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/core/id_set.hpp"
#include "epycbench/core/quantity.hpp"

#include <compare>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::topology {

using ProcessingUnitSet = core::SparseIdSet<core::ProcessingUnitId>;

enum class Availability { available, unavailable, unknown, conflicted };
enum class Confidence { reported, derived, inferred, user_supplied, unknown };
enum class ProvenanceSource {
    hwloc,
    linux_sysfs,
    linux_procfs,
    numa_api,
    architecture_specific,
    fixture,
    derived,
    user,
};

struct Evidence {
    ProvenanceSource source{ProvenanceSource::fixture};
    std::string locator;
    Confidence confidence{Confidence::unknown};
    auto operator<=>(const Evidence&) const = default;
};

template <typename T> struct ConflictingValue {
    T value;
    Evidence evidence;

    auto operator==(const ConflictingValue&) const -> bool = default;
};

template <typename T> class Attribute {
  public:
    [[nodiscard]] static auto available(T value, std::vector<Evidence> evidence = {})
        -> Attribute {
        return Attribute{Availability::available, std::move(value), std::move(evidence)};
    }

    [[nodiscard]] static auto unavailable(std::vector<Evidence> evidence = {}) -> Attribute {
        return Attribute{Availability::unavailable, std::nullopt, std::move(evidence)};
    }

    [[nodiscard]] static auto unknown(std::vector<Evidence> evidence = {}) -> Attribute {
        return Attribute{Availability::unknown, std::nullopt, std::move(evidence)};
    }

    [[nodiscard]] static auto conflicted(ConflictingValue<T> first, ConflictingValue<T> second,
                                         std::vector<ConflictingValue<T>> additional = {})
        -> Attribute {
        std::vector<ConflictingValue<T>> conflicts;
        conflicts.reserve(additional.size() + 2);
        conflicts.push_back(std::move(first));
        conflicts.push_back(std::move(second));
        conflicts.insert(conflicts.end(), std::make_move_iterator(additional.begin()),
                         std::make_move_iterator(additional.end()));
        return Attribute{Availability::conflicted, std::nullopt, {}, std::move(conflicts)};
    }

    [[nodiscard]] auto availability() const noexcept -> Availability { return availability_; }
    [[nodiscard]] auto value() const noexcept -> const std::optional<T>& { return value_; }
    [[nodiscard]] auto evidence() const noexcept -> const std::vector<Evidence>& {
        return evidence_;
    }
    [[nodiscard]] auto conflicts() const noexcept -> const std::vector<ConflictingValue<T>>& {
        return conflicts_;
    }

    auto operator==(const Attribute&) const -> bool = default;

  private:
    Attribute(Availability availability, std::optional<T> value, std::vector<Evidence> evidence,
              std::vector<ConflictingValue<T>> conflicts = {})
        : availability_{availability}, value_{std::move(value)}, evidence_{std::move(evidence)},
          conflicts_{std::move(conflicts)} {}

    Availability availability_;
    std::optional<T> value_;
    std::vector<Evidence> evidence_;
    std::vector<ConflictingValue<T>> conflicts_;
};

enum class CacheKind { data, instruction, unified, unknown };

class PciAddress {
  public:
    [[nodiscard]] static auto parse(std::string_view text) -> std::optional<PciAddress>;
    [[nodiscard]] static auto create(std::uint16_t domain, std::uint8_t bus,
                                     std::uint8_t device, std::uint8_t function)
        -> std::optional<PciAddress>;

    [[nodiscard]] constexpr auto domain() const noexcept -> std::uint16_t { return domain_; }
    [[nodiscard]] constexpr auto bus() const noexcept -> std::uint8_t { return bus_; }
    [[nodiscard]] constexpr auto device() const noexcept -> std::uint8_t { return device_; }
    [[nodiscard]] constexpr auto function() const noexcept -> std::uint8_t { return function_; }

    [[nodiscard]] auto to_string() const -> std::string;
    auto operator<=>(const PciAddress&) const = default;

  private:
    constexpr PciAddress(std::uint16_t domain, std::uint8_t bus, std::uint8_t device,
                         std::uint8_t function) noexcept
        : domain_{domain}, bus_{bus}, device_{device}, function_{function} {}

    std::uint16_t domain_{};
    std::uint8_t bus_{};
    std::uint8_t device_{};
    std::uint8_t function_{};
};

struct Machine {
    core::TopologyMachineId id;
    std::string architecture;
    Attribute<std::string> cpu_vendor;
    Attribute<std::string> cpu_model;
    Attribute<core::ByteCount> total_memory;
    std::vector<core::ByteCount> page_sizes;
    core::CpuSet present_cpus;
    core::CpuSet online_cpus;
    Attribute<core::CpuSet> allowed_cpus;
    Attribute<core::CpuSet> effective_cpus;
    core::NodeSet present_nodes;
    core::NodeSet online_nodes;
    Attribute<core::NodeSet> allowed_nodes;
    Attribute<core::NodeSet> effective_nodes;
};

struct Package {
    core::PackageId id;
    Attribute<std::uint32_t> os_id;
};

// Authoritative ownership: each die names its package and optional architecture group.
struct Die {
    core::DieId id;
    core::PackageId package_id;
    Attribute<std::uint32_t> os_id;
    std::optional<core::ArchitectureGroupId> group_id;
};

// Authoritative group membership: groups own their PU sets; PU-to-group views are derived.
struct ArchitectureGroup {
    core::ArchitectureGroupId id;
    core::PackageId package_id;
    std::optional<core::ArchitectureGroupId> parent_group_id;
    std::string kind;
    std::optional<std::string> subtype;
    ProcessingUnitSet member_pus;
    std::vector<Evidence> provenance;
};

// Authoritative structural ownership: each core names its package and optional die.
struct Core {
    core::CoreId id;
    core::PackageId package_id;
    std::optional<core::DieId> die_id;
    Attribute<std::uint32_t> os_id;
};

// Authoritative PU ownership: each PU names exactly one core; core membership is derived.
struct ProcessingUnit {
    core::ProcessingUnitId id;
    core::LinuxCpuId linux_cpu_id;
    core::CoreId core_id;
    Attribute<bool> isolated;
};

// Authoritative cache membership: each cache owns its PU set; PU-to-cache views are derived.
struct CacheDomain {
    core::CacheId id;
    std::uint32_t level{};
    CacheKind kind{CacheKind::unknown};
    core::ByteCount size;
    core::ByteCount line_size;
    Attribute<std::uint32_t> associativity;
    ProcessingUnitSet member_pus;
    std::vector<Evidence> provenance;
};

// Authoritative NUMA membership: each node owns its PU set; PU-to-node views are derived.
struct NumaNode {
    core::NumaNodeId id;
    Attribute<std::uint32_t> linux_node_id;
    Attribute<core::ByteCount> installed_memory;
    Attribute<core::ByteCount> available_memory;
    ProcessingUnitSet member_pus;
    std::vector<core::ByteCount> page_sizes;
};

struct NumaDistance {
    core::NumaNodeId from;
    core::NumaNodeId to;
    std::uint32_t value{};

    auto operator<=>(const NumaDistance&) const = default;
};

struct NumaDistanceMatrix {
    std::vector<core::NumaNodeId> nodes;
    std::vector<NumaDistance> directed_distances;

    [[nodiscard]] auto distance(core::NumaNodeId from, core::NumaNodeId to) const
        -> std::optional<std::uint32_t>;
};

// PCI address is the snapshot identity. Locality is authored only on the device.
struct PciDevice {
    PciAddress address;
    std::uint16_t vendor_id{};
    std::uint16_t device_id{};
    std::uint32_t class_code{};
    Attribute<core::NumaNodeId> numa_locality;
    Attribute<ProcessingUnitSet> local_pus;
};

enum class TopologyConstructionErrorCode {
    duplicate_package_id,
    duplicate_die_id,
    duplicate_group_id,
    duplicate_core_id,
    duplicate_processing_unit_id,
    duplicate_linux_cpu_id,
    duplicate_cache_id,
    duplicate_numa_node_id,
    duplicate_pci_address,
    duplicate_distance_node_id,
    duplicate_distance_entry,
};

struct TopologyConstructionError {
    TopologyConstructionErrorCode code;
    std::string object_type;
    std::string identifier;

    auto operator<=>(const TopologyConstructionError&) const = default;
};

struct TopologySnapshotData {
    Machine machine;
    std::vector<Package> packages;
    std::vector<Die> dies;
    std::vector<ArchitectureGroup> architecture_groups;
    std::vector<Core> cores;
    std::vector<ProcessingUnit> processing_units;
    std::vector<CacheDomain> caches;
    std::vector<NumaNode> numa_nodes;
    NumaDistanceMatrix numa_distances;
    std::vector<PciDevice> pci_devices;
};

class TopologySnapshot {
  public:
    [[nodiscard]] static auto create(TopologySnapshotData data)
        -> std::expected<TopologySnapshot, TopologyConstructionError>;

    [[nodiscard]] auto machine() const noexcept -> const Machine& { return machine_; }
    [[nodiscard]] auto packages() const noexcept -> const std::vector<Package>& { return packages_; }
    [[nodiscard]] auto dies() const noexcept -> const std::vector<Die>& { return dies_; }
    [[nodiscard]] auto architecture_groups() const noexcept
        -> const std::vector<ArchitectureGroup>& { return architecture_groups_; }
    [[nodiscard]] auto cores() const noexcept -> const std::vector<Core>& { return cores_; }
    [[nodiscard]] auto processing_units() const noexcept
        -> const std::vector<ProcessingUnit>& { return processing_units_; }
    [[nodiscard]] auto caches() const noexcept -> const std::vector<CacheDomain>& { return caches_; }
    [[nodiscard]] auto numa_nodes() const noexcept -> const std::vector<NumaNode>& {
        return numa_nodes_;
    }
    [[nodiscard]] auto numa_distances() const noexcept -> const NumaDistanceMatrix& {
        return numa_distances_;
    }
    [[nodiscard]] auto pci_devices() const noexcept -> const std::vector<PciDevice>& {
        return pci_devices_;
    }

    [[nodiscard]] auto package(core::PackageId id) const -> std::optional<Package>;
    [[nodiscard]] auto die(core::DieId id) const -> std::optional<Die>;
    [[nodiscard]] auto architecture_group(core::ArchitectureGroupId id) const
        -> std::optional<ArchitectureGroup>;
    [[nodiscard]] auto core(core::CoreId id) const -> std::optional<Core>;
    [[nodiscard]] auto processing_unit(core::ProcessingUnitId id) const
        -> std::optional<ProcessingUnit>;
    [[nodiscard]] auto cache(core::CacheId id) const -> std::optional<CacheDomain>;
    [[nodiscard]] auto numa_node(core::NumaNodeId id) const -> std::optional<NumaNode>;
    [[nodiscard]] auto pci_device(PciAddress address) const -> std::optional<PciDevice>;

    [[nodiscard]] auto core_members(core::CoreId id) const -> ProcessingUnitSet;
    [[nodiscard]] auto smt_width(core::CoreId id) const -> std::uint64_t;
    [[nodiscard]] auto package_cores(core::PackageId id) const -> std::vector<core::CoreId>;
    [[nodiscard]] auto package_dies(core::PackageId id) const -> std::vector<core::DieId>;
    [[nodiscard]] auto groups_for_pu(core::ProcessingUnitId id) const
        -> std::vector<core::ArchitectureGroupId>;
    [[nodiscard]] auto caches_for_pu(core::ProcessingUnitId id) const
        -> std::vector<core::CacheId>;
    [[nodiscard]] auto numa_nodes_for_pu(core::ProcessingUnitId id) const
        -> std::vector<core::NumaNodeId>;

  private:
    explicit TopologySnapshot(TopologySnapshotData data);

    Machine machine_;
    std::vector<Package> packages_;
    std::vector<Die> dies_;
    std::vector<ArchitectureGroup> architecture_groups_;
    std::vector<Core> cores_;
    std::vector<ProcessingUnit> processing_units_;
    std::vector<CacheDomain> caches_;
    std::vector<NumaNode> numa_nodes_;
    NumaDistanceMatrix numa_distances_;
    std::vector<PciDevice> pci_devices_;

    std::map<core::CoreId, ProcessingUnitSet> core_members_;
    std::map<core::PackageId, std::vector<core::CoreId>> package_cores_;
    std::map<core::PackageId, std::vector<core::DieId>> package_dies_;
    std::map<core::ProcessingUnitId, std::vector<core::ArchitectureGroupId>> pu_groups_;
    std::map<core::ProcessingUnitId, std::vector<core::CacheId>> pu_caches_;
    std::map<core::ProcessingUnitId, std::vector<core::NumaNodeId>> pu_numa_nodes_;
};

[[nodiscard]] auto format_topology_id(core::TopologyMachineId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::PackageId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::DieId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::ArchitectureGroupId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::CoreId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::ProcessingUnitId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::CacheId id) -> std::string;
[[nodiscard]] auto format_topology_id(core::NumaNodeId id) -> std::string;

} // namespace epycbench::topology
