#include "runtime/controller_lifecycle_policy.hpp"

#include <limits>

namespace epycbench::runtime::detail {

auto controller_lifecycle_policy_v1() noexcept -> ControllerLifecycleConfig {
    using Milliseconds = std::chrono::milliseconds;
    static_assert(std::numeric_limits<Milliseconds::rep>::max() >= 3'600'000);
    return {
        .startup_timeout = Milliseconds{30'000},
        .execution_timeout = Milliseconds{3'600'000},
        .cancellation_grace = Milliseconds{5'000},
        .completion_grace = Milliseconds{300'000},
        .termination_grace = Milliseconds{5'000},
        .observation_quantum = Milliseconds{10},
    };
}

} // namespace epycbench::runtime::detail
