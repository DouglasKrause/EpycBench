#pragma once

#include "output_task_module_support.hpp"

namespace epycbench::module_detail {

inline auto prepare_copy_bytes(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->input_bytes == 0U ||
        request->input_bytes != request->output_bytes || request->xor_mask != 0U ||
        request->input_bytes > std::numeric_limits<std::uint64_t>::max() / 2U ||
        request->input_bytes > std::numeric_limits<std::size_t>::max() / 2U || request->repetitions == 0U)
        return 1;
    if (request->repetitions > std::numeric_limits<std::uint64_t>::max() / request->input_bytes ||
        byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes))
        return 1;
    return 0;
}

[[gnu::always_inline]] inline void copy_byte_tail(const unsigned char* input, unsigned char* output,
                                                  std::size_t offset, std::size_t count) noexcept {
    for (; offset < count; ++offset) output[offset] = input[offset];
}

[[gnu::always_inline]] inline auto copied_last_byte(const unsigned char* output,
                                                     std::size_t count) noexcept -> std::uint64_t {
    // Keep every repetition's stores observable to the compiler. This barrier
    // emits no CPU fence or other instruction.
    asm volatile("" ::: "memory");
    return output[count - 1U];
}

} // namespace epycbench::module_detail
