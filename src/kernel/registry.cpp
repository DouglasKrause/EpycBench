#include "epycbench/kernel/registry.hpp"

#include <array>
#include <cstddef>

namespace epycbench::kernel {

struct detail::KernelDescriptorRegistryAccess {
    [[nodiscard]] static constexpr auto create(
        KernelId kernel_id, KernelImplementationId implementation_id) noexcept
        -> KernelDescriptor {
        return {kernel_id, implementation_id};
    }
};

namespace {

inline constexpr std::array descriptors{
    detail::KernelDescriptorRegistryAccess::create(
        KernelId::sequential_read_bandwidth_v1,
        KernelImplementationId::scalar_u64_sequential_v1),
    detail::KernelDescriptorRegistryAccess::create(
        KernelId::dependent_load_latency_v1,
        KernelImplementationId::scalar_u64_random_cycle_v1),
};

template <typename Projection>
[[nodiscard]] consteval auto unique_by(Projection projection) -> bool {
    for (std::size_t left = 0; left < descriptors.size(); ++left) {
        for (std::size_t right = left + 1U; right < descriptors.size(); ++right) {
            if (projection(descriptors[left]) == projection(descriptors[right])) {
                return false;
            }
        }
    }
    return true;
}

static_assert(descriptors.size() == 2U);
static_assert(unique_by([](const KernelDescriptor& descriptor) {
    return descriptor.kernel_id();
}));
static_assert(unique_by([](const KernelDescriptor& descriptor) {
    return descriptor.implementation_id();
}));

} // namespace

auto kernel_implementation_id_token(KernelImplementationId id) noexcept
    -> std::string_view {
    switch (id) {
    case KernelImplementationId::scalar_u64_sequential_v1:
        return "scalar-u64-sequential/v1";
    case KernelImplementationId::scalar_u64_random_cycle_v1:
        return "scalar-u64-random-cycle/v1";
    }
    return {};
}

auto find_kernel_descriptor(KernelId id) noexcept -> const KernelDescriptor* {
    switch (id) {
    case KernelId::sequential_read_bandwidth_v1:
        return &descriptors[0];
    case KernelId::dependent_load_latency_v1:
        return &descriptors[1];
    }
    return nullptr;
}

} // namespace epycbench::kernel
