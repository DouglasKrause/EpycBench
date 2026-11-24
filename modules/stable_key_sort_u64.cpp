#include "epycbench/kernel/stable_key_sort_u64_task.hpp"
#include "output_task_module_support.hpp"
#include <array>

namespace {
using namespace epycbench;
constexpr auto partitions = kernel::stable_key_sort_u64_partitions;

auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    using namespace module_detail;
    if (!request || !request->input || !request->output || request->xor_mask ||
        !request->repetitions || request->input_bytes < 16U ||
        load_sum_word(request->input, 0) != kernel::stable_key_sort_u64_input_magic)
        return 1;
    const auto n = load_sum_word(request->input, 1U);
    return !n || n > kernel::stable_key_sort_u64_maximum_records ||
           request->input_bytes != 8U * (n + 2U) || request->output_bytes != 8U * n ||
           request->repetitions > UINT64_MAX / (n > partitions ? n : partitions) ||
           byte_ranges_overlap(request->input, request->input_bytes, request->output,
                               request->output_bytes);
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_stable_key_sort_u64_body(const unsigned char* input, unsigned char* output,
                                        std::size_t records, std::uint64_t repetitions) noexcept
    -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
#if EPYCBENCH_STABLE_KEY_SORT_COUNT
        std::array<std::size_t, partitions> next{};
        for (std::size_t i = 0; i < records; ++i)
            ++next[load_sum_word(input, 2U + i) >> 60U];
        std::size_t start{};
        for (auto& count : next) {
            const auto size = count;
            count = start;
            start += size;
        }
        for (std::size_t i = 0; i < records; ++i) {
            const auto word = load_sum_word(input, 2U + i);
            store_xor_word(output, next[word >> 60U]++, word);
        }
#else
        std::size_t position{};
        for (std::size_t key = 0; key < partitions; ++key) {
            for (std::size_t i = 0; i < records; ++i) {
                const auto word = load_sum_word(input, 2U + i);
                if ((word >> 60U) == key)
                    store_xor_word(output, position++, word);
            }
        }
#endif
        for (std::size_t i = 0; i < records; ++i)
            folded += (i + 1U) * load_sum_word(output, i);
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
    *answer = epycbench_stable_key_sort_u64_body(
        request->input, request->output, load_sum_word(request->input, 1U), request->repetitions);
    return 0;
}
