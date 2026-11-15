#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id_set.hpp"

#include <expected>

namespace epycbench::platform_linux {

[[nodiscard]] auto get_current_thread_affinity() -> std::expected<core::CpuSet, core::Error>;

[[nodiscard]] auto set_current_thread_affinity(const core::CpuSet& cpus)
    -> std::expected<void, core::Error>;

} // namespace epycbench::platform_linux
