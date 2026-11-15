#pragma once

#include "epycbench/runtime/controller_lifecycle.hpp"

#include <string_view>

namespace epycbench::runtime::detail {

inline constexpr std::string_view controller_lifecycle_policy_v1_id =
    "epycbench.controller-lifecycle-policy/v1";

// Explicit caller selection only. Reusable engines never select or substitute
// this policy, and a modified copy no longer represents this named profile.
[[nodiscard]] auto controller_lifecycle_policy_v1() noexcept -> ControllerLifecycleConfig;

} // namespace epycbench::runtime::detail
