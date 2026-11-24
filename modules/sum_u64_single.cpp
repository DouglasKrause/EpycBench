#include "task_module_support.hpp"

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (!checksum || epycbench::module_detail::prepare_sum_u64(request) != 0) return 1;
    const auto count = static_cast<std::size_t>(request->bytes / sizeof(std::uint64_t));
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < request->repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t sum{};
        for (std::size_t index{}; index < count; ++index)
            sum += epycbench::module_detail::load_sum_word(request->data, index);
        folded += sum;
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}
