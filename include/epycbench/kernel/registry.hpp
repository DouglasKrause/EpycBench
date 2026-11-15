#pragma once

#include "epycbench/kernel/descriptor.hpp"

#include <expected>
#include <span>

namespace epycbench::kernel {

enum class RegistryError { already_installed, invalid_descriptor, duplicate_id, allocation };

// Install the immutable process catalog before starting workers. It owns copies
// of descriptors; its references remain valid until process exit.
[[nodiscard]] auto install_kernel_descriptors(std::span<const KernelDescriptor>) noexcept
    -> std::expected<void, RegistryError>;

[[nodiscard]] auto find_kernel_descriptor(const KernelId& id) noexcept
    -> const KernelDescriptor*;

} // namespace epycbench::kernel
