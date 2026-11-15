#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/topology/source_evidence.hpp"

#include <expected>
#include <filesystem>

namespace epycbench::topology {

struct LinuxTopologyPaths {
    std::filesystem::path sysfs_root{"/sys"};
    std::filesystem::path procfs_root{"/proc"};
    std::filesystem::path cgroup_root{"/sys/fs/cgroup"};
};

[[nodiscard]] auto discover_linux_topology(const LinuxTopologyPaths& paths = {})
    -> std::expected<TopologySourceEvidence, core::Error>;

} // namespace epycbench::topology
