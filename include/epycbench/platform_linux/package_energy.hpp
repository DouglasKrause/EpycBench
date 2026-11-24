#pragma once

#include "epycbench/core/quantity.hpp"
#include "epycbench/platform_linux/file_descriptor.hpp"
#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include <filesystem>
#include <optional>

namespace epycbench::platform_linux {

namespace detail {
struct MicrojoulesTag;
class PackageEnergyZoneAccess;
}
using Microjoules = core::IntegerQuantity<detail::MicrojoulesTag>;

struct PackageEnergyReading {
    std::uint32_t linux_package_number;
    Microjoules raw_energy;
    Microjoules advertised_maximum;
    RawMonotonicTimestamp read_started;
    RawMonotonicTimestamp read_finished;
};

struct PackageEnergySourceSnapshot {
    std::optional<RawMonotonicTimestamp> started;
    std::optional<RawMonotonicTimestamp> finished;
    std::optional<std::string> common_build_id;
    std::optional<std::string> msr_build_id;
    std::optional<std::string> boot_sha256;
    std::optional<std::uint32_t> amd_family_model;
    std::optional<std::uint64_t> suspend_success;
    std::optional<std::uint64_t> suspend_fail;
    std::optional<std::uint64_t> boottime_ns;
    std::optional<std::uint64_t> attribute_device;
    std::optional<std::uint64_t> attribute_inode;
    std::optional<Microjoules> current_maximum;
};

class PackageEnergyZone {
  public:
    // The topology owner supplies the observed Linux package number and
    // the selected powercap directory. The zone label must agree.
    [[nodiscard]] static auto open(const std::filesystem::path& directory, std::uint32_t linux_package_number)
        -> std::expected<PackageEnergyZone, core::Error>;
    [[nodiscard]] static auto discover(std::uint32_t linux_package_number)
        -> std::expected<PackageEnergyZone, core::Error>;

    PackageEnergyZone(PackageEnergyZone&&) noexcept = default;
    auto operator=(PackageEnergyZone&&) noexcept -> PackageEnergyZone& = default;
    PackageEnergyZone(const PackageEnergyZone&) = delete;
    auto operator=(const PackageEnergyZone&) -> PackageEnergyZone& = delete;

    [[nodiscard]] auto sample() const -> std::expected<PackageEnergyReading, core::Error>;
    [[nodiscard]] auto source_snapshot() const -> PackageEnergySourceSnapshot;
    [[nodiscard]] auto zone_locator() const noexcept -> std::string_view { return zone_locator_; }

  private:
    PackageEnergyZone(FileDescriptor energy, FileDescriptor directory,
                      std::uint32_t package_number, Microjoules maximum,
                      std::string zone_locator = {}) noexcept
        : energy_{std::move(energy)}, directory_{std::move(directory)}, package_number_{package_number}, maximum_{maximum},
          zone_locator_{std::move(zone_locator)} {}

    FileDescriptor energy_;
    FileDescriptor directory_;
    std::uint32_t package_number_;
    Microjoules maximum_;
    std::string zone_locator_;

    friend class detail::PackageEnergyZoneAccess;
};

} // namespace epycbench::platform_linux
