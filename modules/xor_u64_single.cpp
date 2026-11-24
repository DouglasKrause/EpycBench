#include "output_task_module_support.hpp"

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto
epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                            std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_xor_u64(request) != 0 ||
        !epycbench::module_detail::valid_xor_checksum(*request, checksum))
        return 1;
    const auto count = static_cast<std::size_t>(request->input_bytes / sizeof(std::uint64_t));
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < request->repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t sum{};
        for (std::size_t index{}; index < count; ++index) {
            const auto value =
                epycbench::module_detail::load_sum_word(request->input, index) ^ request->xor_mask;
            epycbench::module_detail::store_xor_word(request->output, index, value);
            sum += value;
        }
        folded += sum;
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}
