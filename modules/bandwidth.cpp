#include "epycbench/kernel/detail/memory_work.hpp"
#include "module_support.hpp"

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v1(
    const EpycBenchKernelRequestV1* request) noexcept -> std::uint64_t {
    return epycbench::kernel::detail::sequential_read_work(request->data,
        static_cast<std::size_t>(request->bytes / sizeof(std::uint64_t)), request->work_units);
}

extern "C" [[gnu::visibility("default")]] auto epycbench_kernel_v1() noexcept
    -> const EpycBenchKernelV1* {
    static const EpycBenchKernelV1 descriptor{
        EPYCBENCH_KERNEL_ABI_VERSION, sizeof(EpycBenchKernelV1), EPYCBENCH_KERNEL_BANDWIDTH,
        "memory.bandwidth.read/v1", "scalar-u64-sequential/v1", "1",
        EPYCBENCH_PLUGIN_COMPILER, EPYCBENCH_PLUGIN_BUILD_INFO, "epycbench_kernel_execute_v1",
        epycbench::module_detail::prepare_bandwidth, epycbench_kernel_execute_v1,
        epycbench::module_detail::verify, epycbench::module_detail::cleanup};
    return &descriptor;
}
