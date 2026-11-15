#pragma once

#include "epycbench/platform_linux/dynamic_cpu_mask.hpp"
#include "epycbench/platform_linux/thread_affinity.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace epycbench::platform_linux::detail {

struct RawLinuxResult {
    std::int64_t value;
    int error_number;
};

struct ThreadOperations {
    void* context;
    RawLinuxResult (*get_thread_id)(void*) noexcept;
    RawLinuxResult (*get_current_cpu)(void*) noexcept;
    RawLinuxResult (*get_affinity)(void*, NativeCpuMaskView) noexcept;
    RawLinuxResult (*set_affinity)(void*, ConstNativeCpuMaskView) noexcept;
};

struct CpuMaskOperations {
    void* context;
    std::expected<DynamicCpuMask, core::Error> (*with_capacity)(void*, std::size_t);
    std::expected<DynamicCpuMask, core::Error> (*from_cpu_set)(void*, const core::CpuSet&);
};

struct ThreadAffinityDependencies {
    ThreadOperations threads;
    CpuMaskOperations masks;
};

[[nodiscard]] auto production_thread_operations() noexcept -> ThreadOperations;
[[nodiscard]] auto production_cpu_mask_operations() noexcept -> CpuMaskOperations;

[[nodiscard]] auto thread_error(std::string_view code, std::string_view message,
                                std::string_view operation, std::string_view classification,
                                std::optional<int> error_number = std::nullopt,
                                std::string_view underlying_code = {}) -> core::Error;
[[nodiscard]] auto contained_thread_exception(std::string_view operation) -> core::Error;
[[nodiscard]] auto contained_unknown_thread_failure(std::string_view operation) -> core::Error;

class ThreadIdentityAccess {
  public:
    [[nodiscard]] static auto from_validated(std::uint64_t value) noexcept -> LinuxThreadId {
        return LinuxThreadId{value};
    }
    [[nodiscard]] static auto current_thread_id(const ThreadOperations& operations)
        -> std::expected<LinuxThreadId, core::Error>;
    [[nodiscard]] static auto current_cpu(const ThreadOperations& operations)
        -> std::expected<core::LinuxCpuId, core::Error>;
};

class ThreadAffinityAccess {
  public:
    [[nodiscard]] static auto
    get_current_thread_affinity(const ThreadAffinityDependencies& dependencies)
        -> std::expected<core::CpuSet, core::Error>;
    [[nodiscard]] static auto
    set_current_thread_affinity(const core::CpuSet& cpus,
                                const ThreadAffinityDependencies& dependencies)
        -> std::expected<void, core::Error>;
};

} // namespace epycbench::platform_linux::detail
