#include "epycbench/topology/model.hpp"

#include <algorithm>
#include <charconv>
#include <functional>
#include <ranges>
#include <tuple>

namespace epycbench::topology {
namespace {

template <typename Id>
auto format_id(std::string_view prefix, Id id) -> std::string {
    return std::string{prefix} + '/' + std::to_string(id.value());
}

template <typename Object, typename Projection, typename Formatter>
auto sort_and_reject_duplicates(std::vector<Object>& objects, Projection projection,
                                TopologyConstructionErrorCode code, std::string_view object_type,
                                Formatter formatter) -> std::optional<TopologyConstructionError> {
    std::ranges::sort(objects, {}, projection);
    const auto duplicate = std::ranges::adjacent_find(objects, {}, projection);
    if (duplicate == objects.end()) {
        return std::nullopt;
    }
    return TopologyConstructionError{code, std::string{object_type},
                                     formatter(std::invoke(projection, *duplicate))};
}

} // namespace

auto PciAddress::parse(std::string_view text) -> std::optional<PciAddress> {
    if (text.size() != 12 || text[4] != ':' || text[7] != ':' || text[10] != '.') {
        return std::nullopt;
    }
    const auto parse_hex = [text](std::size_t first, std::size_t last, auto& value) {
        const auto result = std::from_chars(text.data() + first, text.data() + last, value, 16);
        return result.ec == std::errc{} && result.ptr == text.data() + last;
    };

    unsigned int domain{};
    unsigned int bus{};
    unsigned int device{};
    unsigned int function{};
    if (!parse_hex(0, 4, domain) || !parse_hex(5, 7, bus) || !parse_hex(8, 10, device) ||
        !parse_hex(11, 12, function) || domain > 0xffffU || bus > 0xffU || device > 0x1fU ||
        function > 7U) {
        return std::nullopt;
    }
    return create(static_cast<std::uint16_t>(domain), static_cast<std::uint8_t>(bus),
                  static_cast<std::uint8_t>(device), static_cast<std::uint8_t>(function));
}

auto PciAddress::create(std::uint16_t domain, std::uint8_t bus, std::uint8_t device,
                        std::uint8_t function) -> std::optional<PciAddress> {
    if (device > 0x1fU || function > 7U) {
        return std::nullopt;
    }
    return PciAddress{domain, bus, device, function};
}

auto PciAddress::to_string() const -> std::string {
    std::string result(12, '0');
    result[4] = ':';
    result[7] = ':';
    result[10] = '.';
    const auto write_hex = [&result](std::size_t offset, std::size_t count, std::uint32_t value) {
        constexpr std::string_view hex{"0123456789abcdef"};
        for (std::size_t index = 0; index < count; ++index) {
            result[offset + count - index - 1] = hex[value & 0xfU];
            value >>= 4U;
        }
    };
    write_hex(0, 4, domain_);
    write_hex(5, 2, bus_);
    write_hex(8, 2, device_);
    write_hex(11, 1, function_);
    return result;
}

auto NumaDistanceMatrix::distance(core::NumaNodeId from, core::NumaNodeId to) const
    -> std::optional<std::uint32_t> {
    const auto key = std::pair{from, to};
    const auto found = std::ranges::lower_bound(
        directed_distances, key, {}, [](const NumaDistance& entry) {
            return std::pair{entry.from, entry.to};
        });
    if (found == directed_distances.end() || std::pair{found->from, found->to} != key) {
        return std::nullopt;
    }
    return found->value;
}

auto TopologySnapshot::create(TopologySnapshotData data)
    -> std::expected<TopologySnapshot, TopologyConstructionError> {
    TopologySnapshot result{std::move(data)};

    const auto check = [&]<typename Object, typename Projection, typename Formatter>(
                           std::vector<Object>& objects, Projection projection,
                           TopologyConstructionErrorCode code, std::string_view object_type,
                           Formatter formatter) -> std::optional<TopologyConstructionError> {
        return sort_and_reject_duplicates(objects, projection, code, object_type, formatter);
    };

    if (auto error = check(result.packages_, &Package::id,
                           TopologyConstructionErrorCode::duplicate_package_id, "package",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    if (auto error = check(result.dies_, &Die::id,
                           TopologyConstructionErrorCode::duplicate_die_id, "die",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    if (auto error = check(result.architecture_groups_, &ArchitectureGroup::id,
                           TopologyConstructionErrorCode::duplicate_group_id,
                           "architecture_group",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    if (auto error = check(result.cores_, &Core::id,
                           TopologyConstructionErrorCode::duplicate_core_id, "core",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    if (auto error = check(result.processing_units_, &ProcessingUnit::id,
                           TopologyConstructionErrorCode::duplicate_processing_unit_id,
                           "processing_unit",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    {
        auto linux_ids = result.processing_units_;
        if (auto error = check(linux_ids, &ProcessingUnit::linux_cpu_id,
                               TopologyConstructionErrorCode::duplicate_linux_cpu_id,
                               "linux_cpu", [](auto id) { return std::to_string(id.value()); })) {
            return std::unexpected{std::move(*error)};
        }
    }
    if (auto error = check(result.caches_, &CacheDomain::id,
                           TopologyConstructionErrorCode::duplicate_cache_id, "cache",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    if (auto error = check(result.numa_nodes_, &NumaNode::id,
                           TopologyConstructionErrorCode::duplicate_numa_node_id, "numa_node",
                           [](auto id) { return format_topology_id(id); })) {
        return std::unexpected{std::move(*error)};
    }
    if (auto error = check(result.pci_devices_, &PciDevice::address,
                           TopologyConstructionErrorCode::duplicate_pci_address, "pci_device",
                           [](auto address) { return address.to_string(); })) {
        return std::unexpected{std::move(*error)};
    }

    std::ranges::sort(result.numa_distances_.nodes);
    const auto duplicate_distance_node =
        std::ranges::adjacent_find(result.numa_distances_.nodes);
    if (duplicate_distance_node != result.numa_distances_.nodes.end()) {
        return std::unexpected{TopologyConstructionError{
            TopologyConstructionErrorCode::duplicate_distance_node_id, "numa_distance_node",
            format_topology_id(*duplicate_distance_node)}};
    }
    std::ranges::sort(result.numa_distances_.directed_distances, {}, [](const NumaDistance& entry) {
        return std::pair{entry.from, entry.to};
    });
    const auto duplicate_distance = std::ranges::adjacent_find(
        result.numa_distances_.directed_distances, {}, [](const NumaDistance& entry) {
            return std::pair{entry.from, entry.to};
        });
    if (duplicate_distance != result.numa_distances_.directed_distances.end()) {
        return std::unexpected{TopologyConstructionError{
            TopologyConstructionErrorCode::duplicate_distance_entry, "numa_distance",
            format_topology_id(duplicate_distance->from) + "->" +
                format_topology_id(duplicate_distance->to)}};
    }

    for (const auto& pu : result.processing_units_) {
        result.core_members_[pu.core_id] =
            result.core_members_[pu.core_id].set_union(ProcessingUnitSet{pu.id});
    }
    for (const auto& core : result.cores_) {
        result.package_cores_[core.package_id].push_back(core.id);
    }
    for (const auto& die : result.dies_) {
        result.package_dies_[die.package_id].push_back(die.id);
    }
    for (const auto& group : result.architecture_groups_) {
        for (const auto pu : group.member_pus) {
            result.pu_groups_[pu].push_back(group.id);
        }
    }
    for (const auto& cache : result.caches_) {
        for (const auto pu : cache.member_pus) {
            result.pu_caches_[pu].push_back(cache.id);
        }
    }
    for (const auto& node : result.numa_nodes_) {
        for (const auto pu : node.member_pus) {
            result.pu_numa_nodes_[pu].push_back(node.id);
        }
    }
    return result;
}

TopologySnapshot::TopologySnapshot(TopologySnapshotData data)
    : machine_{std::move(data.machine)}, packages_{std::move(data.packages)},
      dies_{std::move(data.dies)}, architecture_groups_{std::move(data.architecture_groups)},
      cores_{std::move(data.cores)}, processing_units_{std::move(data.processing_units)},
      caches_{std::move(data.caches)}, numa_nodes_{std::move(data.numa_nodes)},
      numa_distances_{std::move(data.numa_distances)},
      pci_devices_{std::move(data.pci_devices)} {}

template <typename Object, typename Id, typename Projection>
auto find_by_id(const std::vector<Object>& objects, Id id, Projection projection)
    -> std::optional<Object> {
    const auto found = std::ranges::lower_bound(objects, id, {}, projection);
    return found != objects.end() && std::invoke(projection, *found) == id
               ? std::optional<Object>{*found}
               : std::nullopt;
}

auto TopologySnapshot::package(core::PackageId id) const -> std::optional<Package> {
    return find_by_id(packages_, id, &Package::id);
}
auto TopologySnapshot::die(core::DieId id) const -> std::optional<Die> {
    return find_by_id(dies_, id, &Die::id);
}
auto TopologySnapshot::architecture_group(core::ArchitectureGroupId id) const
    -> std::optional<ArchitectureGroup> {
    return find_by_id(architecture_groups_, id, &ArchitectureGroup::id);
}
auto TopologySnapshot::core(core::CoreId id) const -> std::optional<Core> {
    return find_by_id(cores_, id, &Core::id);
}
auto TopologySnapshot::processing_unit(core::ProcessingUnitId id) const
    -> std::optional<ProcessingUnit> {
    return find_by_id(processing_units_, id, &ProcessingUnit::id);
}
auto TopologySnapshot::cache(core::CacheId id) const -> std::optional<CacheDomain> {
    return find_by_id(caches_, id, &CacheDomain::id);
}
auto TopologySnapshot::numa_node(core::NumaNodeId id) const -> std::optional<NumaNode> {
    return find_by_id(numa_nodes_, id, &NumaNode::id);
}
auto TopologySnapshot::pci_device(PciAddress address) const -> std::optional<PciDevice> {
    return find_by_id(pci_devices_, address, &PciDevice::address);
}

auto TopologySnapshot::core_members(core::CoreId id) const -> ProcessingUnitSet {
    const auto found = core_members_.find(id);
    return found == core_members_.end() ? ProcessingUnitSet{} : found->second;
}
auto TopologySnapshot::smt_width(core::CoreId id) const -> std::uint64_t {
    return core_members(id).cardinality();
}
auto TopologySnapshot::package_cores(core::PackageId id) const
    -> std::vector<core::CoreId> {
    const auto found = package_cores_.find(id);
    return found == package_cores_.end() ? std::vector<core::CoreId>{} : found->second;
}
auto TopologySnapshot::package_dies(core::PackageId id) const
    -> std::vector<core::DieId> {
    const auto found = package_dies_.find(id);
    return found == package_dies_.end() ? std::vector<core::DieId>{} : found->second;
}
auto TopologySnapshot::groups_for_pu(core::ProcessingUnitId id) const
    -> std::vector<core::ArchitectureGroupId> {
    const auto found = pu_groups_.find(id);
    return found == pu_groups_.end() ? std::vector<core::ArchitectureGroupId>{} : found->second;
}
auto TopologySnapshot::caches_for_pu(core::ProcessingUnitId id) const
    -> std::vector<core::CacheId> {
    const auto found = pu_caches_.find(id);
    return found == pu_caches_.end() ? std::vector<core::CacheId>{} : found->second;
}
auto TopologySnapshot::numa_nodes_for_pu(core::ProcessingUnitId id) const
    -> std::vector<core::NumaNodeId> {
    const auto found = pu_numa_nodes_.find(id);
    return found == pu_numa_nodes_.end() ? std::vector<core::NumaNodeId>{} : found->second;
}

auto format_topology_id(core::TopologyMachineId id) -> std::string { return format_id("machine", id); }
auto format_topology_id(core::PackageId id) -> std::string { return format_id("package", id); }
auto format_topology_id(core::DieId id) -> std::string { return format_id("die", id); }
auto format_topology_id(core::ArchitectureGroupId id) -> std::string { return format_id("group", id); }
auto format_topology_id(core::CoreId id) -> std::string { return format_id("core", id); }
auto format_topology_id(core::ProcessingUnitId id) -> std::string { return format_id("pu", id); }
auto format_topology_id(core::CacheId id) -> std::string { return format_id("cache", id); }
auto format_topology_id(core::NumaNodeId id) -> std::string { return format_id("numa", id); }

} // namespace epycbench::topology
