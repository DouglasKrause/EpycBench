#pragma once

#include "epycbench/platform_linux/package_energy.hpp"

namespace epycbench::platform_linux::detail {

class PackageEnergyZoneAccess final {
  public:
    [[nodiscard]] static auto discover(const std::filesystem::path& root,
                                       std::uint32_t linux_package_number)
        -> std::expected<PackageEnergyZone, core::Error>;
    [[nodiscard]] static auto snapshot(const PackageEnergyZone&, const std::filesystem::path& proc,
        const std::filesystem::path& sys) -> PackageEnergySourceSnapshot;
};

} // namespace epycbench::platform_linux::detail
