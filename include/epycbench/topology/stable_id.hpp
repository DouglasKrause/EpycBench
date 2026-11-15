#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"

#include <compare>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epycbench::topology {

namespace detail {

enum class StableIdKind { machine, package, die, group, core, processing_unit, cache, numa, pci };

struct StableMachineTag;
struct StablePackageTag;
struct StableDieTag;
struct StableGroupTag;
struct StableCoreTag;
struct StableProcessingUnitTag;
struct StableCacheTag;
struct StableNumaNodeTag;
struct StablePciDeviceTag;
struct PreparedStableTopologyIdAccess;

[[nodiscard]] auto parse_stable_topology_id(StableIdKind kind, std::string_view text)
    -> std::expected<std::string, core::Error>;

template <typename Tag> inline constexpr StableIdKind stable_id_kind{};
template <> inline constexpr auto stable_id_kind<StableMachineTag> = StableIdKind::machine;
template <> inline constexpr auto stable_id_kind<StablePackageTag> = StableIdKind::package;
template <> inline constexpr auto stable_id_kind<StableDieTag> = StableIdKind::die;
template <> inline constexpr auto stable_id_kind<StableGroupTag> = StableIdKind::group;
template <> inline constexpr auto stable_id_kind<StableCoreTag> = StableIdKind::core;
template <>
inline constexpr auto stable_id_kind<StableProcessingUnitTag> = StableIdKind::processing_unit;
template <> inline constexpr auto stable_id_kind<StableCacheTag> = StableIdKind::cache;
template <> inline constexpr auto stable_id_kind<StableNumaNodeTag> = StableIdKind::numa;
template <> inline constexpr auto stable_id_kind<StablePciDeviceTag> = StableIdKind::pci;

} // namespace detail

class StableTopologyIds;

template <typename Tag> class StableTopologyId {
  public:
    [[nodiscard]] static auto parse(std::string_view text)
        -> std::expected<StableTopologyId, core::Error> {
        auto parsed = detail::parse_stable_topology_id(detail::stable_id_kind<Tag>, text);
        if (!parsed) {
            return std::unexpected{std::move(parsed.error())};
        }
        return StableTopologyId{std::move(*parsed)};
    }

    [[nodiscard]] auto value() const noexcept -> std::string_view { return value_; }

    auto operator<=>(const StableTopologyId&) const = default;

  private:
    explicit StableTopologyId(std::string value) : value_{std::move(value)} {}

    std::string value_;

    friend class StableTopologyIds;
    friend struct detail::PreparedStableTopologyIdAccess;
};

using StableMachineId = StableTopologyId<detail::StableMachineTag>;
using StablePackageId = StableTopologyId<detail::StablePackageTag>;
using StableDieId = StableTopologyId<detail::StableDieTag>;
using StableGroupId = StableTopologyId<detail::StableGroupTag>;
using StableCoreId = StableTopologyId<detail::StableCoreTag>;
using StableProcessingUnitId = StableTopologyId<detail::StableProcessingUnitTag>;
using StableCacheId = StableTopologyId<detail::StableCacheTag>;
using StableNumaNodeId = StableTopologyId<detail::StableNumaNodeTag>;
using StablePciDeviceId = StableTopologyId<detail::StablePciDeviceTag>;

template <typename Handle, typename SemanticId> struct StableTopologyIdEntry {
    Handle handle;
    SemanticId semantic_id;

    auto operator<=>(const StableTopologyIdEntry&) const = default;
};

using StableMachineEntry = StableTopologyIdEntry<core::TopologyMachineId, StableMachineId>;
using StablePackageEntry = StableTopologyIdEntry<core::PackageId, StablePackageId>;
using StableDieEntry = StableTopologyIdEntry<core::DieId, StableDieId>;
using StableGroupEntry = StableTopologyIdEntry<core::ArchitectureGroupId, StableGroupId>;
using StableCoreEntry = StableTopologyIdEntry<core::CoreId, StableCoreId>;
using StableProcessingUnitEntry =
    StableTopologyIdEntry<core::ProcessingUnitId, StableProcessingUnitId>;
using StableCacheEntry = StableTopologyIdEntry<core::CacheId, StableCacheId>;
using StableNumaNodeEntry = StableTopologyIdEntry<core::NumaNodeId, StableNumaNodeId>;
using StablePciDeviceEntry = StableTopologyIdEntry<PciAddress, StablePciDeviceId>;

class StableTopologyIds {
  public:
    [[nodiscard]] auto machine_id() const noexcept -> const StableMachineId&;

    [[nodiscard]] auto package_id(core::PackageId handle) const
        -> std::optional<std::reference_wrapper<const StablePackageId>>;
    [[nodiscard]] auto die_id(core::DieId handle) const
        -> std::optional<std::reference_wrapper<const StableDieId>>;
    [[nodiscard]] auto group_id(core::ArchitectureGroupId handle) const
        -> std::optional<std::reference_wrapper<const StableGroupId>>;
    [[nodiscard]] auto core_id(core::CoreId handle) const
        -> std::optional<std::reference_wrapper<const StableCoreId>>;
    [[nodiscard]] auto processing_unit_id(core::ProcessingUnitId handle) const
        -> std::optional<std::reference_wrapper<const StableProcessingUnitId>>;
    [[nodiscard]] auto cache_id(core::CacheId handle) const
        -> std::optional<std::reference_wrapper<const StableCacheId>>;
    [[nodiscard]] auto numa_node_id(core::NumaNodeId handle) const
        -> std::optional<std::reference_wrapper<const StableNumaNodeId>>;
    [[nodiscard]] auto pci_device_id(PciAddress handle) const
        -> std::optional<std::reference_wrapper<const StablePciDeviceId>>;

    [[nodiscard]] auto machine(const StableMachineId& id) const
        -> std::optional<core::TopologyMachineId>;
    [[nodiscard]] auto package(const StablePackageId& id) const -> std::optional<core::PackageId>;
    [[nodiscard]] auto die(const StableDieId& id) const -> std::optional<core::DieId>;
    [[nodiscard]] auto group(const StableGroupId& id) const
        -> std::optional<core::ArchitectureGroupId>;
    [[nodiscard]] auto core(const StableCoreId& id) const -> std::optional<core::CoreId>;
    [[nodiscard]] auto processing_unit(const StableProcessingUnitId& id) const
        -> std::optional<core::ProcessingUnitId>;
    [[nodiscard]] auto cache(const StableCacheId& id) const -> std::optional<core::CacheId>;
    [[nodiscard]] auto numa_node(const StableNumaNodeId& id) const
        -> std::optional<core::NumaNodeId>;
    [[nodiscard]] auto pci_device(const StablePciDeviceId& id) const -> std::optional<PciAddress>;

    [[nodiscard]] auto machines() const noexcept -> std::span<const StableMachineEntry>;
    [[nodiscard]] auto packages() const noexcept -> std::span<const StablePackageEntry>;
    [[nodiscard]] auto dies() const noexcept -> std::span<const StableDieEntry>;
    [[nodiscard]] auto groups() const noexcept -> std::span<const StableGroupEntry>;
    [[nodiscard]] auto cores() const noexcept -> std::span<const StableCoreEntry>;
    [[nodiscard]] auto processing_units() const noexcept
        -> std::span<const StableProcessingUnitEntry>;
    [[nodiscard]] auto caches() const noexcept -> std::span<const StableCacheEntry>;
    [[nodiscard]] auto numa_nodes() const noexcept -> std::span<const StableNumaNodeEntry>;
    [[nodiscard]] auto pci_devices() const noexcept -> std::span<const StablePciDeviceEntry>;

  private:
    template <typename Handle, typename SemanticId> struct Index {
        std::vector<StableTopologyIdEntry<Handle, SemanticId>> entries;
        std::map<Handle, std::size_t> forward;
        std::map<SemanticId, Handle> reverse;
    };

    template <typename SemanticId>
    [[nodiscard]] static auto make_id(std::string text) -> SemanticId {
        return SemanticId{std::move(text)};
    }

    Index<core::TopologyMachineId, StableMachineId> machines_;
    Index<core::PackageId, StablePackageId> packages_;
    Index<core::DieId, StableDieId> dies_;
    Index<core::ArchitectureGroupId, StableGroupId> groups_;
    Index<core::CoreId, StableCoreId> cores_;
    Index<core::ProcessingUnitId, StableProcessingUnitId> processing_units_;
    Index<core::CacheId, StableCacheId> caches_;
    Index<core::NumaNodeId, StableNumaNodeId> numa_nodes_;
    Index<PciAddress, StablePciDeviceId> pci_devices_;

    friend struct detail::PreparedStableTopologyIdAccess;
    friend auto derive_stable_topology_ids(const TopologySnapshot& topology)
        -> std::expected<StableTopologyIds, core::Error>;
};

[[nodiscard]] auto derive_stable_topology_ids(const TopologySnapshot& topology)
    -> std::expected<StableTopologyIds, core::Error>;

} // namespace epycbench::topology
