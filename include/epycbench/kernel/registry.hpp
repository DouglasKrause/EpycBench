#pragma once

#include "epycbench/kernel/descriptor.hpp"

namespace epycbench::kernel {

[[nodiscard]] auto find_kernel_descriptor(KernelId id) noexcept
    -> const KernelDescriptor*;

} // namespace epycbench::kernel
