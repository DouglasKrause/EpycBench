#pragma once

#include "output_task_module_support.hpp"

namespace epycbench::module_detail {
static_assert(sizeof(double) == 8U && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53);

inline auto prepare_affine_f64(const EpycBenchKernelRequestV3* request) noexcept -> int {
    // The existing XOR helper checks only generic equal-size, disjoint, positive
    // input/output ranges and checked N*R work. Affine has no runtime coefficient.
    if (prepare_xor_u64(request) != 0 || request->xor_mask != 0U) return 1;
    return 0;
}
[[gnu::always_inline]] inline auto affine_f64_value(const unsigned char* input,
                                                  std::size_t index) noexcept -> double {
    return std::bit_cast<double>(load_sum_word(input, index));
}
[[gnu::always_inline]] inline void store_affine_f64(unsigned char* output,
                                                  std::size_t index, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    std::memcpy(output + index * sizeof(value), &value, sizeof(value));
}
} // namespace epycbench::module_detail
