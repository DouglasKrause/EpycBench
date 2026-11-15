#pragma once

#include "epycbench/topology/stable_id.hpp"

#include <string>
#include <utility>

namespace epycbench::topology::detail {

// Inactive prepared-codec seam.  Callers must validate the canonical NUMA
// grammar before transferring an already-capacity-backed owning string.
struct PreparedStableTopologyIdAccess final {
    [[nodiscard]] static auto derive_with_failure_sink(const TopologySnapshot&,
        std::optional<core::Error>*) -> std::expected<StableTopologyIds, core::Error>;
    [[nodiscard]] static auto adopt_numa(std::string value) noexcept -> StableNumaNodeId {
        return StableNumaNodeId{std::move(value)};
    }
};

} // namespace epycbench::topology::detail
