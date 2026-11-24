#include "epycbench/kernel/csr_u64_task.hpp"
#include "output_task_module_support.hpp"

#include <algorithm>

namespace {
using namespace epycbench;
constexpr auto header = kernel::csr_u64_header_words;
auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->xor_mask ||
        !request->repetitions || request->input_bytes < 8U * header ||
        module_detail::load_sum_word(request->input, 0U) != kernel::csr_u64_input_magic)
        return 1;
    const auto rows = module_detail::load_sum_word(request->input, 1U);
    const auto columns = module_detail::load_sum_word(request->input, 2U);
    const auto nonzeros = module_detail::load_sum_word(request->input, 3U);
    if (!rows || rows > kernel::csr_u64_maximum_rows || !columns ||
        columns > kernel::csr_u64_maximum_columns || nonzeros > 4U * rows ||
        request->input_bytes != 8U * (header + rows + 1U + 2U * nonzeros + columns) ||
        request->output_bytes != 8U * (header + rows) ||
        request->repetitions > UINT64_MAX / std::max(rows, nonzeros) ||
        module_detail::byte_ranges_overlap(request->input, request->input_bytes, request->output,
                                           request->output_bytes))
        return 1;
    return 0;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_csr_u64_body(const unsigned char* input, unsigned char* output,
                            std::uint64_t repetitions, std::uint64_t* checksum) noexcept -> int {
    using namespace epycbench::module_detail;
    const auto rows = load_sum_word(input, 1U), columns = load_sum_word(input, 2U),
               nonzeros = load_sum_word(input, 3U);
    const auto indices = header + rows + 1U, coefficients = indices + nonzeros,
               vector = coefficients + nonzeros;
    std::uint64_t folded{};
    for (std::uint64_t rep{}; rep < repetitions; ++rep) {
        asm volatile("" ::: "memory");
        for (std::size_t i{}; i < header; ++i)
            store_xor_word(output, i, load_sum_word(input, i));
        if (load_sum_word(input, header) != 0U || load_sum_word(input, header + rows) != nonzeros)
            return 1;
        for (std::size_t row{}; row < rows; ++row) {
            auto pos = load_sum_word(input, header + row);
            const auto end = load_sum_word(input, header + row + 1U);
            if (pos > end || end > nonzeros)
                return 1;
            std::uint64_t value{};
            const auto add = [&](std::uint64_t i) {
                const auto col = load_sum_word(input, indices + i);
                if (col >= columns)
                    return false;
                value +=
                    load_sum_word(input, coefficients + i) * load_sum_word(input, vector + col);
                return true;
            };
#if EPYCBENCH_CSR_UNROLL4
            for (; end - pos >= 4U; pos += 4U)
                if (!add(pos) || !add(pos + 1U) || !add(pos + 2U) || !add(pos + 3U))
                    return 1;
#endif
            for (; pos < end; ++pos)
                if (!add(pos))
                    return 1;
            store_xor_word(output, header + row, value);
            folded += value;
        }
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                                 std::uint64_t* answer) noexcept -> int {
    if (prepare(request) != 0 || !epycbench::module_detail::valid_xor_checksum(*request, answer))
        return 1;
    return epycbench_csr_u64_body(request->input, request->output, request->repetitions, answer);
}
