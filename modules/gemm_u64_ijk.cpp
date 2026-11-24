#include "gemm_u64_module_support.hpp"

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_gemm_u64_ijk_body(const unsigned char* input, unsigned char* output,
                                 std::size_t rows, std::size_t inner, std::size_t columns,
                                 std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    const auto* b = input + rows * inner * 8U;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t answer{};
        for (std::size_t i{}; i < rows; ++i)
            for (std::size_t j{}; j < columns; ++j) {
                std::uint64_t value{};
                for (std::size_t k{}; k < inner; ++k)
                    value +=
                        load_sum_word(input, i * inner + k) * load_sum_word(b, k * columns + j);
                store_xor_word(output, i * columns + j, value);
                answer += value;
            }
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
    *checksum = epycbench_gemm_u64_ijk_body(
        request->input + epycbench::kernel::gemm_u64_header_bytes, request->output, shape.rows,
        shape.inner, shape.columns, request->repetitions);
    return 0;
}
