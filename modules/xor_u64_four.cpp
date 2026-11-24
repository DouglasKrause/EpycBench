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
        std::uint64_t a0{}, a1{}, a2{}, a3{};
        std::size_t index{};
        for (; count - index >= 4U; index += 4U) {
            const auto v0 =
                epycbench::module_detail::load_sum_word(request->input, index) ^ request->xor_mask;
            const auto v1 = epycbench::module_detail::load_sum_word(request->input, index + 1U) ^
                            request->xor_mask;
            const auto v2 = epycbench::module_detail::load_sum_word(request->input, index + 2U) ^
                            request->xor_mask;
            const auto v3 = epycbench::module_detail::load_sum_word(request->input, index + 3U) ^
                            request->xor_mask;
            epycbench::module_detail::store_xor_word(request->output, index, v0);
            epycbench::module_detail::store_xor_word(request->output, index + 1U, v1);
            epycbench::module_detail::store_xor_word(request->output, index + 2U, v2);
            epycbench::module_detail::store_xor_word(request->output, index + 3U, v3);
            a0 += v0;
            a1 += v1;
            a2 += v2;
            a3 += v3;
        }
        for (; index < count; ++index) {
            const auto value =
                epycbench::module_detail::load_sum_word(request->input, index) ^ request->xor_mask;
            epycbench::module_detail::store_xor_word(request->output, index, value);
            a0 += value;
        }
        folded += a0 + a1 + a2 + a3;
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}
