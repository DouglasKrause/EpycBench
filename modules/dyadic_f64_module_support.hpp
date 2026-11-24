#pragma once

#include "task_module_support.hpp"

namespace epycbench::module_detail {
static_assert(sizeof(double) == 8U && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53);

inline auto prepare_dyadic_f64(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (prepare_sum_u64(request) != 0) return 1;
    return request->bytes / 8U <= (UINT64_C(1) << 20U) ? 0 : 1;
}
inline auto valid_dyadic_f64_checksum(const EpycBenchKernelRequestV2& request,
                                      const std::uint64_t* checksum) noexcept -> bool {
    if (!checksum) return false;
    const auto input = reinterpret_cast<std::uintptr_t>(request.data);
    const auto output = reinterpret_cast<std::uintptr_t>(checksum);
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    return request.bytes <= maximum - input && sizeof(*checksum) <= maximum - output &&
        !(input < output + sizeof(*checksum) && output < input + request.bytes);
}
[[gnu::always_inline]] inline auto load_dyadic_f64(
    const unsigned char* input, std::size_t index) noexcept -> double {
    return std::bit_cast<double>(load_sum_word(input, index));
}
[[gnu::always_inline]] inline auto load_dyadic_f64_magnitude(
    const unsigned char* input, std::size_t index) noexcept -> double {
    return std::bit_cast<double>(load_sum_word(input, index) & UINT64_C(0x7fffffffffffffff));
}
[[gnu::always_inline]] inline auto canonical_dyadic_f64_bits(double value) noexcept -> std::uint64_t {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    return (bits & UINT64_C(0x7fffffffffffffff)) == 0U ? 0U : bits;
}
} // namespace epycbench::module_detail
