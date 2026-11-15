#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"
#include "epycbench/topology/stable_id.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace epycbench::topology::detail {

// Private profile-byte seam for golden tests; this is not topology serialization.
[[nodiscard]] auto canonical_physical_fingerprint_projection(const TopologySnapshot&,
                                                             const StableTopologyIds&)
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto canonical_effective_fingerprint_projection(const TopologySnapshot&,
                                                              const StableTopologyIds&,
                                                              std::string_view physical_fingerprint)
    -> std::expected<std::string, core::Error>;

} // namespace epycbench::topology::detail
