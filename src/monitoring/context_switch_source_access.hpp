#pragma once
#include "epycbench/monitoring/context_switches.hpp"
namespace epycbench::monitoring::detail {
class ContextSwitchSourceAccess {
  public:
    using DrainOperation =
        std::expected<platform_linux::PerfContextSwitchDrain, core::Error> (*)(void*) noexcept;
    static auto create(core::WorkerId worker_id, void* context, DrainOperation drain) noexcept
        -> ContextSwitchEventSource {
        return ContextSwitchEventSource{worker_id, context, drain, nullptr};
    }
};
} // namespace epycbench::monitoring::detail
