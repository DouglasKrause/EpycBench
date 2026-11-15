#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id_set.hpp"

#include <cstddef>
#include <expected>
#include <sched.h>

namespace epycbench::platform_linux {

struct NativeCpuMaskView {
    cpu_set_t* data;
    std::size_t size;
};

struct ConstNativeCpuMaskView {
    const cpu_set_t* data;
    std::size_t size;
};

namespace detail {
class DynamicCpuMaskAccess;
}

class DynamicCpuMask {
  public:
    [[nodiscard]] static auto from_cpu_set(const core::CpuSet& cpus)
        -> std::expected<DynamicCpuMask, core::Error>;

    [[nodiscard]] static auto with_capacity(std::size_t minimum_capacity)
        -> std::expected<DynamicCpuMask, core::Error>;

    DynamicCpuMask(DynamicCpuMask&& other) noexcept;
    auto operator=(DynamicCpuMask&& other) noexcept -> DynamicCpuMask&;

    DynamicCpuMask(const DynamicCpuMask&) = delete;
    auto operator=(const DynamicCpuMask&) -> DynamicCpuMask& = delete;

    ~DynamicCpuMask() noexcept;

    void zero() noexcept;

    [[nodiscard]] auto set(core::LinuxCpuId cpu) noexcept -> bool;
    [[nodiscard]] auto clear(core::LinuxCpuId cpu) noexcept -> bool;
    [[nodiscard]] auto contains(core::LinuxCpuId cpu) const noexcept -> bool;

    [[nodiscard]] auto to_cpu_set() const -> std::expected<core::CpuSet, core::Error>;

    [[nodiscard]] auto native_view() noexcept -> NativeCpuMaskView;
    [[nodiscard]] auto native_view() const noexcept -> ConstNativeCpuMaskView;

    [[nodiscard]] auto native_size() const noexcept -> std::size_t;
    [[nodiscard]] auto capacity() const noexcept -> std::size_t;

    // Independent masks may be used concurrently. Concurrent const access to one mask requires
    // that no mutation or writable native view is active; all mutation requires external
    // synchronization. Native views remain valid only while this owner retains its allocation.

  private:
    using ReleaseFunction = void (*)(void*, cpu_set_t*) noexcept;

    DynamicCpuMask(cpu_set_t* data, std::size_t native_size, std::size_t capacity,
                   void* allocator_context, ReleaseFunction release_function) noexcept;

    void release() noexcept;

    cpu_set_t* data_{nullptr};
    std::size_t native_size_{0};
    std::size_t capacity_{0};
    void* allocator_context_{nullptr};
    ReleaseFunction release_{nullptr};

    friend class detail::DynamicCpuMaskAccess;
};

} // namespace epycbench::platform_linux
