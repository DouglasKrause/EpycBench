#pragma once

#include "task_module_support.hpp"

namespace epycbench::module_detail {

inline auto byte_ranges_overlap(const void* first, std::uint64_t first_bytes, const void* second,
                                std::uint64_t second_bytes) noexcept -> bool {
    const auto a = reinterpret_cast<std::uintptr_t>(first);
    const auto b = reinterpret_cast<std::uintptr_t>(second);
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (first_bytes > maximum - a || second_bytes > maximum - b)
        return true;
    return a < b + second_bytes && b < a + first_bytes;
}

inline auto prepare_xor_u64(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->input_bytes == 0U ||
        request->input_bytes != request->output_bytes || request->input_bytes % 8U != 0U ||
        request->input_bytes > std::numeric_limits<std::size_t>::max() ||
        request->repetitions == 0U)
        return 1;
    const auto count = request->input_bytes / sizeof(std::uint64_t);
    if (request->repetitions > std::numeric_limits<std::uint64_t>::max() / count ||
        byte_ranges_overlap(request->input, request->input_bytes, request->output,
                            request->output_bytes))
        return 1;
    return 0;
}

inline auto valid_xor_checksum(const EpycBenchKernelRequestV3& request,
                               const std::uint64_t* checksum) noexcept -> bool {
    return checksum &&
           !byte_ranges_overlap(request.input, request.input_bytes, checksum, sizeof(*checksum)) &&
           !byte_ranges_overlap(request.output, request.output_bytes, checksum, sizeof(*checksum));
}

[[gnu::always_inline]] inline void store_xor_word(unsigned char* output, std::size_t index,
                                                  std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big)
        value = std::byteswap(value);
    std::memcpy(output + index * sizeof(value), &value, sizeof(value));
}

} // namespace epycbench::module_detail
