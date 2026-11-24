#pragma once

#include "epycbench/kernel/gemm_u64_task.hpp"
#include "output_task_module_support.hpp"

namespace epycbench::module_detail {
struct GemmU64Shape {
    std::size_t rows, inner, columns;
};
inline auto prepare_gemm_u64(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output ||
        request->input_bytes < kernel::gemm_u64_header_bytes || request->repetitions == 0U ||
        request->xor_mask != 0U || request->input_bytes > std::numeric_limits<std::size_t>::max() ||
        request->output_bytes > std::numeric_limits<std::size_t>::max() ||
        byte_ranges_overlap(request->input, request->input_bytes, request->output,
                            request->output_bytes))
        return 1;
    if (load_sum_word(request->input, 0U) != kernel::gemm_u64_input_magic)
        return 1;
    const auto rows = load_sum_word(request->input, 1U), inner = load_sum_word(request->input, 2U),
               columns = load_sum_word(request->input, 3U);
    for (const auto dimension : {rows, inner, columns})
        if (!dimension || dimension > kernel::gemm_u64_maximum_dimension)
            return 1;
    if (request->input_bytes !=
            kernel::gemm_u64_header_bytes + (rows * inner + inner * columns) * 8U ||
        request->output_bytes != rows * columns * 8U ||
        request->repetitions > std::numeric_limits<std::uint64_t>::max() / (rows * inner * columns))
        return 1;
    return 0;
}
inline auto gemm_u64_shape(const unsigned char* input) noexcept -> GemmU64Shape {
    return {static_cast<std::size_t>(load_sum_word(input, 1U)),
            static_cast<std::size_t>(load_sum_word(input, 2U)),
            static_cast<std::size_t>(load_sum_word(input, 3U))};
}
} // namespace epycbench::module_detail
