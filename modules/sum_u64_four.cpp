#include "task_module_support.hpp"

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (!checksum || epycbench::module_detail::prepare_sum_u64(request) != 0) return 1;
    const auto count = static_cast<std::size_t>(request->bytes / sizeof(std::uint64_t));
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < request->repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t a0{}, a1{}, a2{}, a3{};
        std::size_t index{};
        for (; count - index >= 4U; index += 4U) {
            a0 += epycbench::module_detail::load_sum_word(request->data, index);
            a1 += epycbench::module_detail::load_sum_word(request->data, index + 1U);
            a2 += epycbench::module_detail::load_sum_word(request->data, index + 2U);
            a3 += epycbench::module_detail::load_sum_word(request->data, index + 3U);
        }
        for (; index < count; ++index)
            a0 += epycbench::module_detail::load_sum_word(request->data, index);
        folded += a0 + a1 + a2 + a3;
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}
