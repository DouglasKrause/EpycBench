#include "epycbench/kernel/transpose_u64_task.hpp"
#include "output_task_module_support.hpp"

#include <algorithm>

namespace {
using namespace epycbench;
constexpr std::size_t header = kernel::transpose_u64_header_words;

auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->xor_mask != 0U ||
        request->input_bytes < 8U * header || request->input_bytes != request->output_bytes ||
        request->repetitions == 0U || module_detail::load_sum_word(request->input, 0U) != kernel::transpose_u64_input_magic)
        return 1;
    const auto rows = module_detail::load_sum_word(request->input, 1U);
    const auto columns = module_detail::load_sum_word(request->input, 2U);
    if (!rows || !columns || rows > kernel::transpose_u64_maximum_dimension || columns > kernel::transpose_u64_maximum_dimension)
        return 1;
    const auto elements = rows * columns;
    if (request->input_bytes != 8U * (header + elements) || request->repetitions > UINT64_MAX / elements ||
        module_detail::byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes))
        return 1;
    return 0;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_transpose_u64_body(const unsigned char* input, unsigned char* output,
                                  std::size_t rows, std::size_t columns, std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        store_xor_word(output, 0U, epycbench::kernel::transpose_u64_input_magic);
        store_xor_word(output, 1U, columns);
        store_xor_word(output, 2U, rows);
        const auto transpose_word = [&](std::size_t row, std::size_t column) {
            const auto value = load_sum_word(input, header + row * columns + column);
            store_xor_word(output, header + column * rows + row, value);
            folded += value;
        };
#if EPYCBENCH_TRANSPOSE_TILED
        constexpr std::size_t tile = 8U;
        for (std::size_t first_row{}; first_row < rows; first_row += tile)
            for (std::size_t first_column{}; first_column < columns; first_column += tile)
                for (auto row = first_row; row < std::min(first_row + tile, rows); ++row)
                    for (auto column = first_column; column < std::min(first_column + tile, columns); ++column)
                        transpose_word(row, column);
#else
        for (std::size_t row{}; row < rows; ++row)
            for (std::size_t column{}; column < columns; ++column)
                transpose_word(row, column);
#endif
    }
    asm volatile("" ::: "memory");
    return folded;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request, std::uint64_t* answer) noexcept -> int {
    using namespace epycbench::module_detail;
    if (prepare(request) != 0 || !valid_xor_checksum(*request, answer)) return 1;
    *answer = epycbench_transpose_u64_body(request->input, request->output,
        static_cast<std::size_t>(load_sum_word(request->input, 1U)),
        static_cast<std::size_t>(load_sum_word(request->input, 2U)), request->repetitions);
    return 0;
}
