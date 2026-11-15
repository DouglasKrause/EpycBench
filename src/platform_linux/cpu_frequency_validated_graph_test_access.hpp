#pragma once

#include "platform_linux/cpu_frequency_access.hpp"

namespace epycbench::platform_linux::detail {

class BoundedCpuFrequencyValidatedGraphOutcomeTestAccess final {
  public:
    [[nodiscard]] static auto supported(
        std::vector<std::size_t> memberships) noexcept
        -> BoundedCpuFrequencyValidatedGraphOutcome {
        return BoundedCpuFrequencyValidatedGraphOutcome{
            BoundedCpuFrequencyValidatedDisposition::supported,
            std::move(memberships)};
    }
    [[nodiscard]] static auto unsupported() noexcept
        -> BoundedCpuFrequencyValidatedGraphOutcome {
        return BoundedCpuFrequencyValidatedGraphOutcome{
            BoundedCpuFrequencyValidatedDisposition::unsupported, {}};
    }
};

} // namespace epycbench::platform_linux::detail
