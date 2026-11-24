#pragma once

#include <chrono>

#include <string_view>

namespace epycbench::runtime::detail {

struct ControllerLifecycleConfig {
    std::chrono::milliseconds startup_timeout;
    std::chrono::milliseconds execution_timeout;
    std::chrono::milliseconds cancellation_grace;
    std::chrono::milliseconds completion_grace;
    std::chrono::milliseconds termination_grace;
    std::chrono::milliseconds observation_quantum;
};

inline constexpr std::string_view controller_lifecycle_policy_v1_id =
    "epycbench.controller-lifecycle-policy/v1";

// Explicit caller selection only. Reusable engines never select or substitute
// this policy, and a modified copy no longer represents this named profile.
[[nodiscard]] auto controller_lifecycle_policy_v1() noexcept -> ControllerLifecycleConfig;

} // namespace epycbench::runtime::detail
