#pragma once

#include "epycbench/platform_linux/thread_affinity.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

namespace epycbench::runtime::detail {

struct ServicePlatformOperations {
    void* context{};
    std::expected<platform_linux::LinuxThreadId, core::Error> (*current_thread_id)(void*){};
    std::expected<void, core::Error> (*set_current_thread_affinity)(void*, const core::CpuSet&){};
    std::expected<core::CpuSet, core::Error> (*get_current_thread_affinity)(void*){};
    std::expected<core::LinuxCpuId, core::Error> (*current_cpu)(void*){};
};

[[nodiscard]] auto production_service_platform_operations() noexcept -> ServicePlatformOperations;

} // namespace epycbench::runtime::detail
