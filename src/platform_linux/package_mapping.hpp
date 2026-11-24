#pragma once

#include "platform_linux/package_workers.hpp"

namespace epycbench::platform_linux::package_execution {

struct MappingRank {
    RankTrial rank;
    nlohmann::json mapping;
};

// An independent worker executes one operation and retires within the block.
// Supplied observations replace host placement and clocks. Both modes invoke
// the admitted implementation and oracle with a fresh mapping.
[[nodiscard]] auto run_mapping(const Role&, const nlohmann::json& admitted, const nlohmann::json* supplied,
                               PackageMonitor* monitor = nullptr)
    -> std::expected<MappingRank, core::Error>;

} // namespace epycbench::platform_linux::package_execution
