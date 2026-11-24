#include "gemm_u64_module_support.hpp"

#include <algorithm>

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_gemm_u64_blocked_body(const unsigned char* input, unsigned char* output,
                                     std::size_t rows, std::size_t inner, std::size_t columns,
                                     std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    const auto* b = input + rows * inner * 8U;
    constexpr std::size_t tile = 8U;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        for (std::size_t index{}; index < rows * columns; ++index)
            store_xor_word(output, index, 0U);
        for (std::size_t ii{}; ii < rows; ii += tile)
            for (std::size_t kk{}; kk < inner; kk += tile)
                for (std::size_t jj{}; jj < columns; jj += tile)
                    for (std::size_t i = ii; i < std::min(rows, ii + tile); ++i)
                        for (std::size_t k = kk; k < std::min(inner, kk + tile); ++k) {
                            const auto a = load_sum_word(input, i * inner + k);
                            for (std::size_t j = jj; j < std::min(columns, jj + tile); ++j) {
                                const auto index = i * columns + j;
                                store_xor_word(output, index,
                                               load_sum_word(output, index) +
                                                   a * load_sum_word(b, k * columns + j));
                            }
                        }
        std::uint64_t answer{};
        for (std::size_t index{}; index < rows * columns; ++index)
            answer += load_sum_word(output, index);
        folded += answer;
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto
epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                            std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_gemm_u64(request) != 0 ||
        !epycbench::module_detail::valid_xor_checksum(*request, checksum))
        return 1;
    const auto shape = epycbench::module_detail::gemm_u64_shape(request->input);
    *checksum = epycbench_gemm_u64_blocked_body(
        request->input + epycbench::kernel::gemm_u64_header_bytes, request->output, shape.rows,
        shape.inner, shape.columns, request->repetitions);
    return 0;
}
