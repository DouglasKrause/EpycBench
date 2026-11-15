#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/fingerprint.hpp"
#include "epycbench/topology/model.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace epycbench::topology {

enum class TopologyJsonInputPolicy { accept_equivalent, require_canonical };

struct SerializedTopology {
    std::string canonical_json;
    TopologyFingerprints fingerprints;
};

[[nodiscard]] auto serialize_topology(const TopologySnapshot& topology,
                                      const StableTopologyIds& stable_ids,
                                      const TopologyFingerprints& fingerprints)
    -> std::expected<SerializedTopology, core::Error>;

struct LoadedTopology {
    TopologySnapshot topology;
    StableTopologyIds stable_ids;
    TopologyFingerprints fingerprints;
    std::string canonical_json;
};

[[nodiscard]] auto deserialize_topology(std::string_view json, TopologyJsonInputPolicy policy)
    -> std::expected<LoadedTopology, core::Error>;

} // namespace epycbench::topology
