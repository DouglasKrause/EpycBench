#pragma once

#include "epycbench/platform_linux/dynamic_cpu_mask.hpp"

#include <cstddef>
#include <expected>

namespace epycbench::platform_linux::detail {

struct CpuMaskAllocator {
    void* context;
    cpu_set_t* (*allocate)(void* context, std::size_t capacity);
    void (*release)(void* context, cpu_set_t* data) noexcept;
    std::size_t (*allocation_size)(void* context, std::size_t capacity);
};

[[nodiscard]] auto production_cpu_mask_allocator() noexcept -> CpuMaskAllocator;

class DynamicCpuMaskAccess {
  public:
    [[nodiscard]] static auto adopt(cpu_set_t* data, std::size_t native_size,
                                    std::size_t capacity, void* allocator_context,
                                    DynamicCpuMask::ReleaseFunction release) noexcept
        -> DynamicCpuMask {
        return DynamicCpuMask{data, native_size, capacity, allocator_context, release};
    }
    [[nodiscard]] static auto with_capacity(std::size_t minimum_capacity,
                                            const CpuMaskAllocator& allocator)
        -> std::expected<DynamicCpuMask, core::Error>;
};

} // namespace epycbench::platform_linux::detail
