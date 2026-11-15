#pragma once

#include "epycbench/kernel/plugin.h"

#include <cstdint>

#if defined(__clang__)
#define EPYCBENCH_PLUGIN_COMPILER "Clang " __clang_version__
#elif defined(__GNUC__)
#define EPYCBENCH_PLUGIN_COMPILER "GCC " __VERSION__
#else
#define EPYCBENCH_PLUGIN_COMPILER "unknown"
#endif

#ifndef EPYCBENCH_PLUGIN_BUILD_INFO
#define EPYCBENCH_PLUGIN_BUILD_INFO "C++23"
#endif

namespace epycbench::module_detail {
inline auto prepare_bandwidth(const EpycBenchKernelRequestV1* request) noexcept -> int {
    return request && request->data && request->bytes && request->bytes % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(request->data) % 64U == 0U &&
        request->work_units && request->start_index == 0U ? 0 : 1;
}
inline auto prepare_latency(const EpycBenchKernelRequestV1* request) noexcept -> int {
    return request && request->data && request->bytes >= 128U && request->bytes % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(request->data) % 64U == 0U &&
        request->work_units && request->start_index < request->bytes / 64U ? 0 : 1;
}
inline auto verify(std::uint64_t expected, std::uint64_t actual) noexcept -> int {
    return expected == actual ? 0 : 1;
}
inline void cleanup() noexcept {}
} // namespace epycbench::module_detail
