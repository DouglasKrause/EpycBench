#include "epycbench/kernel/scatter_u64_task.hpp"
#include "output_task_module_support.hpp"

namespace {
using namespace epycbench;
constexpr auto header = kernel::scatter_u64_header_words;

auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->xor_mask || !request->repetitions ||
        request->input_bytes < 8U * header || request->input_bytes != request->output_bytes ||
        module_detail::load_sum_word(request->input, 0U) != kernel::scatter_u64_input_magic)
        return 1;
    const auto count = module_detail::load_sum_word(request->input, 1U);
    if (!count || count > kernel::scatter_u64_maximum_count || request->input_bytes != 8U * (header + count) ||
        request->repetitions > UINT64_MAX / count ||
        module_detail::byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes))
        return 1;
    return 0;
}
}

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_scatter_u64_body(const unsigned char* input, unsigned char* output, std::size_t count,
                                std::uint64_t repetitions, std::uint64_t* checksum) noexcept -> int {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t rep{}; rep < repetitions; ++rep) {
        asm volatile("" ::: "memory");
        for (std::size_t i{}; i < header; ++i) store_xor_word(output, i, load_sum_word(input, i));
        const auto scatter = [&](std::size_t ordinal) {
            const auto word = load_sum_word(input, header + ordinal);
            const auto destination = static_cast<std::size_t>(word >> 48U);
            if (destination >= count) return false;
            store_xor_word(output, header + destination, word);
            folded += word;
            return true;
        };
        std::size_t i{};
#if EPYCBENCH_SCATTER_UNROLL4
        for (; count - i >= 4U; i += 4U)
            if (!scatter(i) || !scatter(i+1U) || !scatter(i+2U) || !scatter(i+3U)) return 1;
#endif
        for (; i < count; ++i)
            if (!scatter(i)) return 1;
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request, std::uint64_t* answer) noexcept -> int {
    using namespace epycbench::module_detail;
    if (prepare(request) != 0 || !valid_xor_checksum(*request, answer)) return 1;
    return epycbench_scatter_u64_body(request->input, request->output,
        static_cast<std::size_t>(load_sum_word(request->input, 1U)), request->repetitions, answer);
}
