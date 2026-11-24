#pragma once

#include "epycbench/core/id.hpp"
#include "epycbench/platform_linux/package_energy.hpp"

namespace epycbench::monitoring {

struct PackageEnergyDelta {
    platform_linux::Microjoules energy;
    core::Duration minimum_elapsed;
    core::Duration maximum_elapsed;
};

class PackageEnergyEvidence {
  public:
    [[nodiscard]] static auto create(core::PackageId package, std::uint32_t linux_package_number,
        platform_linux::PackageEnergyReading before, platform_linux::PackageEnergyReading after,
        platform_linux::RawMonotonicTimestamp enclosing_start,
        platform_linux::RawMonotonicTimestamp enclosing_end)
        -> std::expected<PackageEnergyEvidence, core::Error>;

    [[nodiscard]] auto package_id() const noexcept -> core::PackageId { return package_; }
    [[nodiscard]] auto before() const noexcept -> const platform_linux::PackageEnergyReading& { return before_; }
    [[nodiscard]] auto after() const noexcept -> const platform_linux::PackageEnergyReading& { return after_; }

    // Use only with an independently established upper increment for the
    // complete bracket and continuity of the source counter. The reported
    // maximum need not be its actual wrap modulus.
    [[nodiscard]] auto unwrapped_delta(platform_linux::Microjoules maximum_increment) const noexcept
        -> std::optional<PackageEnergyDelta>;

  private:
    PackageEnergyEvidence(core::PackageId package,
        platform_linux::PackageEnergyReading before, platform_linux::PackageEnergyReading after) noexcept
        : package_{package}, before_{before}, after_{after} {}

    core::PackageId package_;
    platform_linux::PackageEnergyReading before_;
    platform_linux::PackageEnergyReading after_;
};

} // namespace epycbench::monitoring
