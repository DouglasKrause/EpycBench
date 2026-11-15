#include "epycbench/kernel/plugin.h"

#include <cstdint>
#include <cstring>

#ifndef EPYCBENCH_PLUGIN_BUILD_INFO
#define EPYCBENCH_PLUGIN_BUILD_INFO "standalone-example-C++23"
#endif
#if defined(__clang__)
#define EPYCBENCH_EXAMPLE_COMPILER "Clang " __clang_version__
#else
#define EPYCBENCH_EXAMPLE_COMPILER "GCC " __VERSION__
#endif

namespace {
auto prepare(const EpycBenchKernelRequestV1* request) noexcept -> int {
    return request && request->data && request->bytes && request->bytes % 64U == 0U &&
        reinterpret_cast<std::uintptr_t>(request->data) % 64U == 0U &&
        request->work_units && request->start_index == 0U ? 0 : 1;
}
auto verify(std::uint64_t expected, std::uint64_t actual) noexcept -> int {
    return expected == actual ? 0 : 1;
}
void cleanup() noexcept {}
}

// A simple single-accumulator implementation of the same read-bandwidth work
// contract. It needs only plugin.h, not EpycBench's internal C++ libraries.
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v1(
    const EpycBenchKernelRequestV1* request) noexcept -> std::uint64_t {
    std::uint64_t total{};
    auto passes = request->work_units;
    asm volatile("" ::: "memory");
    do {
        for (std::uint64_t offset{}; offset < request->bytes; offset += sizeof(std::uint64_t)) {
            std::uint64_t value;
            std::memcpy(&value, request->data + offset, sizeof(value));
            total += value;
        }
        asm volatile("" ::: "memory");
        --passes;
    } while (passes);
    return total;
}

extern "C" [[gnu::visibility("default")]] auto epycbench_kernel_v1() noexcept
    -> const EpycBenchKernelV1* {
    static const EpycBenchKernelV1 descriptor{
        EPYCBENCH_KERNEL_ABI_VERSION, sizeof(EpycBenchKernelV1), EPYCBENCH_KERNEL_BANDWIDTH,
        "memory.bandwidth.read/example/v1", "example-single-accumulator/v1", "1",
        EPYCBENCH_EXAMPLE_COMPILER, EPYCBENCH_PLUGIN_BUILD_INFO, "epycbench_kernel_execute_v1",
        prepare, epycbench_kernel_execute_v1, verify, cleanup};
    return &descriptor;
}
