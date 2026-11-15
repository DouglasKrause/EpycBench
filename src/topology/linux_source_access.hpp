#pragma once
#include "epycbench/topology/linux_source.hpp"
#include "platform_linux/topology_source_io.hpp"
namespace epycbench::topology::detail {
[[nodiscard]] auto discover_linux_topology_with_prepared(
    platform_linux::detail::PreparedTopologySourceIO&, std::uint64_t original_absolute_end,
    const LinuxTopologyPaths& = {}, std::optional<core::Error>* raw_failure = nullptr) -> std::expected<TopologySourceEvidence, core::Error>;
} // namespace epycbench::topology::detail
