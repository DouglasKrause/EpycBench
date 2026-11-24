#pragma once

#include "epycbench/kernel/merge_u64_task.hpp"
#include "output_task_module_support.hpp"

namespace epycbench::module_detail {
inline auto prepare_merge_u64(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || !request->repetitions ||
        request->input_bytes < 16U || request->input_bytes != request->output_bytes ||
        request->input_bytes % 8U != 0U || request->xor_mask != 0U ||
        request->input_bytes / 8U > kernel::merge_u64_maximum_count ||
        request->repetitions > UINT64_MAX / (request->input_bytes / 8U) ||
        byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes))
        return 1;
    return 0;
}
// First excluded position in the sorted prefix. left=true selects <= boundary
// (left-run ties win), false selects < boundary. Bounds never inspect end.
template<bool left>
[[gnu::always_inline]] inline auto merge_prefix_end(const unsigned char* input, std::size_t begin,
    std::size_t end, std::uint64_t boundary) noexcept -> std::size_t {
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2U;
        const auto word = load_sum_word(input, middle);
        if (left ? word <= boundary : word < boundary) begin = middle + 1U;
        else end = middle;
    }
    return begin;
}
} // namespace epycbench::module_detail
