#pragma once

#include "epycbench/runtime/controller_lifecycle.hpp"
#include "platform_linux/controller_signal_scope.hpp"

namespace epycbench::runtime::detail {
// Explicit borrowed source: the scope must remain alive and active until use ends.
[[nodiscard]] inline auto controller_signal_cancellation(
    platform_linux::detail::ControllerSignalScope& scope) noexcept -> CancellationObserver {
    return {&scope, [](void* context) noexcept {
        using Signal = platform_linux::detail::ControllerSignal;
        switch (static_cast<platform_linux::detail::ControllerSignalScope*>(context)->observe()) {
        case Signal::interrupt: return ControllerCancellation::user_interrupt;
        case Signal::terminate: return ControllerCancellation::controller_shutdown;
        case Signal::none: return ControllerCancellation::none;
        }
        return ControllerCancellation::none;
    }};
}
} // namespace epycbench::runtime::detail
