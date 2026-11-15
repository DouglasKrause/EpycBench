#pragma once

#include "platform_linux/hwmon_temperature_access.hpp"

namespace epycbench::platform_linux::detail {

class BoundedHwmonValidatedOutcomeTestAccess final {
  public:
    [[nodiscard]] static auto supported() noexcept
        -> BoundedHwmonValidatedOutcome {
        return BoundedHwmonValidatedOutcome{
            BoundedHwmonValidatedDisposition::supported};
    }
    [[nodiscard]] static auto unsupported() noexcept
        -> BoundedHwmonValidatedOutcome {
        return BoundedHwmonValidatedOutcome{
            BoundedHwmonValidatedDisposition::unsupported};
    }
};

} // namespace epycbench::platform_linux::detail
