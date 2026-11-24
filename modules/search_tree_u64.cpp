#include "epycbench/kernel/search_tree_u64_task.hpp"
#include "output_task_module_support.hpp"
#include <bit>

namespace {
using namespace epycbench;
constexpr auto leaves = kernel::search_tree_u64_leaves;

auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    using namespace module_detail;
    if (!request || !request->input || !request->output || request->xor_mask ||
        !request->repetitions || request->input_bytes < 16U ||
        load_sum_word(request->input, 0) != kernel::search_tree_u64_input_magic)
        return 1;
    const auto n = load_sum_word(request->input, 1U);
    return n < leaves || n > kernel::search_tree_u64_maximum_queries || !std::has_single_bit(n) ||
           request->input_bytes != 8U * (n + kernel::search_tree_u64_header_words) ||
           request->output_bytes != 8U * n || request->repetitions > UINT64_MAX / (4U * n) ||
           byte_ranges_overlap(request->input, request->input_bytes, request->output,
                               request->output_bytes);
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_search_tree_u64_body(const unsigned char* input, unsigned char* output,
                                    std::size_t queries, std::uint64_t repetitions) noexcept
    -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        for (std::size_t i = 0; i < queries; ++i) {
            const auto query =
                load_sum_word(input, epycbench::kernel::search_tree_u64_header_words + i);
            std::size_t low{};
#if EPYCBENCH_SEARCH_TREE_STEPS
            for (std::size_t step = leaves / 2U; step; step /= 2U)
                low += step *
                       static_cast<std::size_t>(load_sum_word(input, 2U + low + step - 1U) < query);
#else
            std::size_t high = leaves - 1U;
            while (low < high) {
                const auto middle = low + (high - low) / 2U;
                if (load_sum_word(input, 2U + middle) < query)
                    low = middle + 1U;
                else
                    high = middle;
            }
#endif
            store_xor_word(output, i, low);
            folded += (i + 1U) * (low + 1U);
        }
    }
    asm volatile("" ::: "memory");
    return folded;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                                 std::uint64_t* answer) noexcept -> int {
    using namespace epycbench::module_detail;
    if (prepare(request) || !valid_xor_checksum(*request, answer))
        return 1;
    *answer = epycbench_search_tree_u64_body(
        request->input, request->output, load_sum_word(request->input, 1U), request->repetitions);
    return 0;
}
