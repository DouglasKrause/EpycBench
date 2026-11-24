#pragma once
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/stable_id.hpp"

namespace epycbench::kernel::packages { class Catalog; }

namespace epycbench::scenario {
struct ResourceResolutionContext {
    const topology::TopologySnapshot& topology;
    const topology::StableTopologyIds& stable_ids;
    const topology::TopologyFingerprints& fingerprints;
    const kernel::packages::Catalog* packages{};
};

} // namespace epycbench::scenario
