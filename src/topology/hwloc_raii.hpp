#pragma once

#include "epycbench/topology/hwloc_source.hpp"

#include <expected>
#include <hwloc.h>
#include <hwloc/distances.h>
#include <string>
#include <string_view>

namespace epycbench::topology::detail {

// Private conversion seam; production supplies no hooks. Release observers must
// preserve the actual API's nonallocating, noexcept ownership retirement.
struct HwlocDistanceConversionHooks final {
    void* context{};
    int (*get)(void*, hwloc_topology_t, unsigned*, hwloc_distances_s**,
               unsigned long, unsigned long){};
    void (*before_convert)(void*, unsigned){};
    void (*release)(void*, hwloc_topology_t, hwloc_distances_s*) noexcept{};
};

[[nodiscard]] auto convert_hwloc_distances_for_test(
    hwloc_topology_t, TopologySourceEvidence&, const HwlocDistanceConversionHooks&)
    -> std::expected<void, core::Error>;

// Pure seam through the actual warning sorter; performs no hwloc discovery.
void sort_hwloc_diagnostics_for_test(TopologySourceEvidence&);

class HwlocTopology {
  public:
    HwlocTopology() = default;
    ~HwlocTopology();
    HwlocTopology(const HwlocTopology&) = delete;
    auto operator=(const HwlocTopology&) -> HwlocTopology& = delete;
    HwlocTopology(HwlocTopology&& other) noexcept;
    auto operator=(HwlocTopology&& other) noexcept -> HwlocTopology&;

    [[nodiscard]] auto initialize(const HwlocDiscoveryOptions& options)
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto initialize_with(const HwlocDiscoveryOptions& options,
                                       int (*initialize_api)(hwloc_topology_t*))
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto load() -> std::expected<void, core::Error>;
    [[nodiscard]] auto get() const noexcept -> hwloc_topology_t { return value_; }

  private:
    hwloc_topology_t value_{nullptr};
};

[[nodiscard]] auto discover_hwloc_synthetic_for_test(
    std::string_view description, const HwlocDiscoveryOptions& options = {})
    -> std::expected<TopologySourceEvidence, core::Error>;
[[nodiscard]] auto discover_hwloc_xml_for_test(std::string_view xml,
                                                const HwlocDiscoveryOptions& options = {})
    -> std::expected<TopologySourceEvidence, core::Error>;
[[nodiscard]] auto discover_hwloc_xml_reversed_identity_packages_for_test(
    std::string_view xml, const HwlocDiscoveryOptions& options = {})
    -> std::expected<TopologySourceEvidence, core::Error>;
[[nodiscard]] auto export_hwloc_synthetic_xml_for_test(std::string_view description)
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto validate_hwloc_pci_address_for_test(std::uint32_t domain, std::uint32_t bus,
                                                       std::uint32_t device,
                                                       std::uint32_t function)
    -> std::expected<std::string, core::Error>;

} // namespace epycbench::topology::detail
