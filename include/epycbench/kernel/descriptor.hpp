#pragma once

#include "epycbench/kernel/kernel_id.hpp"

#include <cstdint>
#include <string_view>

namespace epycbench::kernel {

namespace detail {
struct KernelDescriptorRegistryAccess;
}

enum class KernelImplementationId : std::uint8_t {
    scalar_u64_sequential_v1,
    scalar_u64_random_cycle_v1,
};

[[nodiscard]] auto kernel_implementation_id_token(KernelImplementationId id) noexcept
    -> std::string_view;

class KernelDescriptor {
  public:
    KernelDescriptor(const KernelDescriptor&) noexcept = default;
    KernelDescriptor(KernelDescriptor&&) noexcept = default;
    auto operator=(const KernelDescriptor&) -> KernelDescriptor& = delete;
    auto operator=(KernelDescriptor&&) -> KernelDescriptor& = delete;
    ~KernelDescriptor() noexcept = default;

    [[nodiscard]] constexpr auto kernel_id() const noexcept -> KernelId {
        return kernel_id_;
    }
    [[nodiscard]] constexpr auto implementation_id() const noexcept
        -> KernelImplementationId {
        return implementation_id_;
    }

  private:
    constexpr KernelDescriptor(KernelId kernel_id,
                               KernelImplementationId implementation_id) noexcept
        : kernel_id_{kernel_id}, implementation_id_{implementation_id} {}

    const KernelId kernel_id_;
    const KernelImplementationId implementation_id_;

    friend struct detail::KernelDescriptorRegistryAccess;
};

} // namespace epycbench::kernel
