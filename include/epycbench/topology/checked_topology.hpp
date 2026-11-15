#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/model.hpp"

#include <expected>

namespace epycbench::topology {

// Performs the production discovery/reconciliation/validation sequence and returns
// one owned snapshot only when every stage has completed successfully.
[[nodiscard]] auto discover_checked_topology() noexcept
    -> std::expected<TopologySnapshot, core::Error>;

} // namespace epycbench::topology
