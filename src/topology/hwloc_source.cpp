#include "epycbench/topology/hwloc_source.hpp"

#include "epycbench/topology/model.hpp"
#include "core/json_ownership.hpp"
#include "topology/hwloc_raii.hpp"
#include "topology/hwloc_source_conversion.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <hwloc/distances.h>
#include <hwloc/export.h>

namespace epycbench::topology {
namespace {

struct InstalledHwlocApi final {
    using ConstBitmap = ::hwloc_const_bitmap_t;
    using Topology = ::hwloc_topology_t;
    using Object = ::hwloc_obj_t;
    using ObjectType = ::hwloc_obj_type_t;
    using CacheType = ::hwloc_obj_cache_type_t;
    using Distance = ::hwloc_distances_s;
    static constexpr auto constant_distances_kind_means_bandwidth = HWLOC_DISTANCES_KIND_MEANS_BANDWIDTH;
    static constexpr auto constant_distances_kind_means_latency = HWLOC_DISTANCES_KIND_MEANS_LATENCY;
    static constexpr auto constant_obj_cache_data = HWLOC_OBJ_CACHE_DATA;
    static constexpr auto constant_obj_cache_instruction = HWLOC_OBJ_CACHE_INSTRUCTION;
    static constexpr auto constant_obj_cache_unified = HWLOC_OBJ_CACHE_UNIFIED;
    static constexpr auto constant_obj_core = HWLOC_OBJ_CORE;
    static constexpr auto constant_obj_die = HWLOC_OBJ_DIE;
    static constexpr auto constant_obj_group = HWLOC_OBJ_GROUP;
    static constexpr auto constant_obj_l1cache = HWLOC_OBJ_L1CACHE;
    static constexpr auto constant_obj_l1icache = HWLOC_OBJ_L1ICACHE;
    static constexpr auto constant_obj_l2cache = HWLOC_OBJ_L2CACHE;
    static constexpr auto constant_obj_l2icache = HWLOC_OBJ_L2ICACHE;
    static constexpr auto constant_obj_l3cache = HWLOC_OBJ_L3CACHE;
    static constexpr auto constant_obj_l3icache = HWLOC_OBJ_L3ICACHE;
    static constexpr auto constant_obj_l4cache = HWLOC_OBJ_L4CACHE;
    static constexpr auto constant_obj_l5cache = HWLOC_OBJ_L5CACHE;
    static constexpr auto constant_obj_memcache = HWLOC_OBJ_MEMCACHE;
    static constexpr auto constant_obj_numanode = HWLOC_OBJ_NUMANODE;
    static constexpr auto constant_obj_package = HWLOC_OBJ_PACKAGE;
    static constexpr auto constant_obj_pci_device = HWLOC_OBJ_PCI_DEVICE;
    static constexpr auto constant_obj_pu = HWLOC_OBJ_PU;
    static constexpr auto constant_unknown_index = HWLOC_UNKNOWN_INDEX;
    static constexpr auto constant_version = HWLOC_VERSION;
    template<class... Args> decltype(auto) bitmap_first(Args&&... args) const noexcept(noexcept(::hwloc_bitmap_first(std::forward<Args>(args)...))) {
        return ::hwloc_bitmap_first(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) bitmap_iszero(Args&&... args) const noexcept(noexcept(::hwloc_bitmap_iszero(std::forward<Args>(args)...))) {
        return ::hwloc_bitmap_iszero(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) bitmap_next(Args&&... args) const noexcept(noexcept(::hwloc_bitmap_next(std::forward<Args>(args)...))) {
        return ::hwloc_bitmap_next(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) bitmap_weight(Args&&... args) const noexcept(noexcept(::hwloc_bitmap_weight(std::forward<Args>(args)...))) {
        return ::hwloc_bitmap_weight(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) distances_get(Args&&... args) const noexcept(noexcept(::hwloc_distances_get(std::forward<Args>(args)...))) {
        return ::hwloc_distances_get(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) distances_get_name(Args&&... args) const noexcept(noexcept(::hwloc_distances_get_name(std::forward<Args>(args)...))) {
        return ::hwloc_distances_get_name(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) distances_release(Args&&... args) const noexcept(noexcept(::hwloc_distances_release(std::forward<Args>(args)...))) {
        return ::hwloc_distances_release(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) get_next_obj_by_type(Args&&... args) const noexcept(noexcept(::hwloc_get_next_obj_by_type(std::forward<Args>(args)...))) {
        return ::hwloc_get_next_obj_by_type(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) get_root_obj(Args&&... args) const noexcept(noexcept(::hwloc_get_root_obj(std::forward<Args>(args)...))) {
        return ::hwloc_get_root_obj(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) obj_get_info_by_name(Args&&... args) const noexcept(noexcept(::hwloc_obj_get_info_by_name(std::forward<Args>(args)...))) {
        return ::hwloc_obj_get_info_by_name(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) obj_type_snprintf(Args&&... args) const noexcept(noexcept(::hwloc_obj_type_snprintf(std::forward<Args>(args)...))) {
        return ::hwloc_obj_type_snprintf(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) topology_get_allowed_cpuset(Args&&... args) const noexcept(noexcept(::hwloc_topology_get_allowed_cpuset(std::forward<Args>(args)...))) {
        return ::hwloc_topology_get_allowed_cpuset(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) topology_get_allowed_nodeset(Args&&... args) const noexcept(noexcept(::hwloc_topology_get_allowed_nodeset(std::forward<Args>(args)...))) {
        return ::hwloc_topology_get_allowed_nodeset(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) topology_get_complete_cpuset(Args&&... args) const noexcept(noexcept(::hwloc_topology_get_complete_cpuset(std::forward<Args>(args)...))) {
        return ::hwloc_topology_get_complete_cpuset(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) topology_get_complete_nodeset(Args&&... args) const noexcept(noexcept(::hwloc_topology_get_complete_nodeset(std::forward<Args>(args)...))) {
        return ::hwloc_topology_get_complete_nodeset(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) topology_get_topology_cpuset(Args&&... args) const noexcept(noexcept(::hwloc_topology_get_topology_cpuset(std::forward<Args>(args)...))) {
        return ::hwloc_topology_get_topology_cpuset(std::forward<Args>(args)...);
    }
    template<class... Args> decltype(auto) topology_get_topology_nodeset(Args&&... args) const noexcept(noexcept(::hwloc_topology_get_topology_nodeset(std::forward<Args>(args)...))) {
        return ::hwloc_topology_get_topology_nodeset(std::forward<Args>(args)...);
    }
    using DistanceHooks = detail::HwlocDistanceConversionHooks;
};
InstalledHwlocApi installed_api;
using Conversion = detail::HwlocSourceConversion<InstalledHwlocApi>;
Conversion converter{installed_api};

auto configure_source(detail::HwlocTopology& topology, const HwlocDiscoveryOptions& options,
                      const auto& configure) -> std::expected<TopologySourceEvidence, core::Error> {
    auto initialized = topology.initialize(options);
    if (!initialized) {
        return std::unexpected{std::move(initialized.error())};
    }
    if (configure(topology.get()) != 0) {
        return std::unexpected{Conversion::error("topology.hwloc.load_failed",
                                     "Unable to configure the hwloc discovery source",
                                     "discover_hwloc_topology")};
    }
    auto loaded = topology.load();
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    return converter.convert(topology.get(), options);
}

} // namespace

namespace detail {

void sort_hwloc_diagnostics_for_test(TopologySourceEvidence& result) {
    Conversion::sort_diagnostics(result);
}

auto convert_hwloc_distances_for_test(hwloc_topology_t topology, TopologySourceEvidence& result,
                                      const HwlocDistanceConversionHooks& hooks)
    -> std::expected<void, core::Error> {
    return converter.convert_distances(topology, result, &hooks);
}

HwlocTopology::~HwlocTopology() {
    if (value_ != nullptr) {
        hwloc_topology_destroy(value_);
    }
}

HwlocTopology::HwlocTopology(HwlocTopology&& other) noexcept
    : value_{std::exchange(other.value_, nullptr)} {}

auto HwlocTopology::operator=(HwlocTopology&& other) noexcept -> HwlocTopology& {
    if (this != &other) {
        if (value_ != nullptr) {
            hwloc_topology_destroy(value_);
        }
        value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
}

auto HwlocTopology::initialize(const HwlocDiscoveryOptions& options)
    -> std::expected<void, core::Error> {
    return initialize_with(options, hwloc_topology_init);
}

auto HwlocTopology::initialize_with(const HwlocDiscoveryOptions& options,
                                    int (*initialize_api)(hwloc_topology_t*))
    -> std::expected<void, core::Error> {
    if (initialize_api(&value_) != 0) {
        return std::unexpected{Conversion::error("topology.hwloc.initialize_failed",
                                     "Unable to initialize hwloc topology discovery",
                                     "discover_hwloc_topology")};
    }
    if (hwloc_topology_set_flags(value_, HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED) != 0 ||
        hwloc_topology_set_all_types_filter(value_, HWLOC_TYPE_FILTER_KEEP_ALL) != 0 ||
        hwloc_topology_set_type_filter(value_, HWLOC_OBJ_OS_DEVICE, HWLOC_TYPE_FILTER_KEEP_NONE) !=
            0 ||
        hwloc_topology_set_type_filter(value_, HWLOC_OBJ_MISC, HWLOC_TYPE_FILTER_KEEP_NONE) != 0 ||
        (!options.include_pci &&
         hwloc_topology_set_type_filter(value_, HWLOC_OBJ_PCI_DEVICE,
                                        HWLOC_TYPE_FILTER_KEEP_NONE) != 0)) {
        return std::unexpected{Conversion::error("topology.hwloc.initialize_failed",
                                     "Unable to configure hwloc topology filters",
                                     "discover_hwloc_topology")};
    }
    return {};
}

auto HwlocTopology::load() -> std::expected<void, core::Error> {
    if (hwloc_topology_load(value_) != 0) {
        return std::unexpected{Conversion::error("topology.hwloc.load_failed",
                                     "Unable to load hwloc topology discovery",
                                     "discover_hwloc_topology")};
    }
    return {};
}

auto discover_hwloc_synthetic_for_test(std::string_view description,
                                       const HwlocDiscoveryOptions& options)
    -> std::expected<TopologySourceEvidence, core::Error> {
    HwlocTopology topology;
    const std::string owned{description};
    return configure_source(topology, options, [&](auto value) {
        return hwloc_topology_set_synthetic(value, owned.c_str());
    });
}

auto discover_hwloc_xml_for_test(std::string_view xml, const HwlocDiscoveryOptions& options)
    -> std::expected<TopologySourceEvidence, core::Error> {
    HwlocTopology topology;
    const std::string owned{xml};
    return configure_source(topology, options, [&](auto value) {
        return hwloc_topology_set_xmlbuffer(value, owned.data(), static_cast<int>(owned.size()));
    });
}

auto discover_hwloc_xml_reversed_identity_packages_for_test(
    std::string_view xml, const HwlocDiscoveryOptions& options)
    -> std::expected<TopologySourceEvidence, core::Error> {
    HwlocTopology topology;
    auto initialized = topology.initialize(options);
    if (!initialized) {
        return std::unexpected{std::move(initialized.error())};
    }
    const std::string owned{xml};
    if (hwloc_topology_set_xmlbuffer(topology.get(), owned.data(), static_cast<int>(owned.size())) !=
        0) {
        return std::unexpected{Conversion::error("topology.hwloc.load_failed",
                                     "Unable to configure the hwloc discovery source",
                                     "discover_hwloc_topology")};
    }
    auto loaded = topology.load();
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    return converter.convert(topology.get(), options, true);
}

auto export_hwloc_synthetic_xml_for_test(std::string_view description)
    -> std::expected<std::string, core::Error> {
    HwlocTopology topology;
    auto initialized = topology.initialize({});
    if (!initialized) {
        return std::unexpected{std::move(initialized.error())};
    }
    const std::string owned{description};
    if (hwloc_topology_set_synthetic(topology.get(), owned.c_str()) != 0) {
        return std::unexpected{Conversion::error("topology.hwloc.load_failed",
                                     "Unable to configure a synthetic hwloc topology",
                                     "discover_hwloc_topology")};
    }
    auto loaded = topology.load();
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    char* buffer = nullptr;
    int length = 0;
    if (hwloc_topology_export_xmlbuffer(topology.get(), &buffer, &length, 0) != 0 ||
        buffer == nullptr || length <= 0) {
        return std::unexpected{Conversion::error("topology.hwloc.load_failed",
                                     "Unable to export a synthetic hwloc topology",
                                     "discover_hwloc_topology")};
    }
    std::string xml{buffer, static_cast<std::size_t>(length - 1)};
    hwloc_free_xmlbuffer(topology.get(), buffer);
    return xml;
}

auto validate_hwloc_pci_address_for_test(std::uint32_t domain, std::uint32_t bus,
                                         std::uint32_t device, std::uint32_t function)
    -> std::expected<std::string, core::Error> {
    if (domain > std::numeric_limits<std::uint16_t>::max() || bus > 0xffU || device > 0x1fU ||
        function > 7U) {
        return std::unexpected{Conversion::error("topology.hwloc.invalid_pci_address",
                                     "A PCI function has an invalid BDF",
                                     "discover_hwloc_topology")};
    }
    const auto address = PciAddress::create(static_cast<std::uint16_t>(domain),
                                            static_cast<std::uint8_t>(bus),
                                            static_cast<std::uint8_t>(device),
                                            static_cast<std::uint8_t>(function));
    if (!address) {
        return std::unexpected{Conversion::error("topology.hwloc.invalid_pci_address",
                                     "A PCI function has an invalid BDF",
                                     "discover_hwloc_topology")};
    }
    return address->to_string();
}

} // namespace detail

auto discover_hwloc_topology(const HwlocDiscoveryOptions& options)
    -> std::expected<TopologySourceEvidence, core::Error> {
    detail::HwlocTopology topology;
    auto initialized = topology.initialize(options);
    if (!initialized) {
        return std::unexpected{std::move(initialized.error())};
    }
    auto loaded = topology.load();
    if (!loaded) {
        return std::unexpected{std::move(loaded.error())};
    }
    return converter.convert(topology.get(), options);
}

} // namespace epycbench::topology
