#pragma once

#include "cli/scenario_topology.hpp"
#include "scenario/package_scenario.hpp"

namespace epycbench::cli::scenario_guides {

struct NumaMatrix {
    // Linux node order fixes the case order. Readers are sorted by Linux CPU.
    std::map<std::uint32_t, scenario_topology::Workers> readers;
    scenario_topology::MemoryNodes targets;
    std::uint64_t minimum_kib{}, maximum_kib{};
};

struct MemoryGuides {
    NumaMatrix matrix;
    std::vector<scenario_topology::CacheSweepSite> cache_sites;
};

auto memory_guides(const kernel::packages::Catalog&, const scenario::ResourceResolutionContext&)
    -> MemoryGuides;

auto guide_controller_choices(const nlohmann::json& execution,
                              const scenario::ResourceResolutionContext&, MemoryGuides&, bool bandwidth)
    -> std::vector<scenario::PackageControllerPlacement>;

auto matrix_readers(const NumaMatrix&, core::CoreId controller)
    -> std::map<std::uint32_t, core::LinuxCpuId>;

auto numa_scenario(const NumaMatrix&, const std::map<std::uint32_t, core::LinuxCpuId>&,
                   std::uint64_t working_set_kib) -> nlohmann::json;

auto cache_scenario(const scenario_topology::CacheSweepSite&, std::uint32_t remote)
    -> nlohmann::json;

} // namespace epycbench::cli::scenario_guides
