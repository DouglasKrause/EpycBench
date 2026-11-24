#include "epycbench/kernel/detail/memory_work.hpp"
#include "module_support.hpp"

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v1(
    const EpycBenchKernelRequestV1* request) noexcept -> std::uint64_t {
    return epycbench::kernel::detail::dependent_load_work(request->data,
        static_cast<std::size_t>(request->start_index), request->work_units);
}
