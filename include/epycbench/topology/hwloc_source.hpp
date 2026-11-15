#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/source_evidence.hpp"

#include <expected>

namespace epycbench::topology {

struct HwlocDiscoveryOptions {
    bool include_pci{true};
    bool include_distances{true};
};

[[nodiscard]] auto discover_hwloc_topology(const HwlocDiscoveryOptions& options = {})
    -> std::expected<TopologySourceEvidence, core::Error>;

} // namespace epycbench::topology
