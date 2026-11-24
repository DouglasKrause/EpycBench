#pragma once

#include "task_module_support.hpp"

namespace epycbench::module_detail {
static_assert(sizeof(double) == 8U && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53);

inline auto prepare_dot_f64(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (!request || !request->data || request->bytes == 0U || request->bytes % 16U != 0U ||
        request->bytes > std::numeric_limits<std::size_t>::max() || request->repetitions == 0U) return 1;
    const auto pairs = request->bytes / 16U;
    return pairs <= (UINT64_C(1) << 20U) &&
        request->repetitions <= std::numeric_limits<std::uint64_t>::max() / pairs ? 0 : 1;
}
inline auto valid_dot_f64_checksum(const EpycBenchKernelRequestV2& request,
                                  const std::uint64_t* checksum) noexcept -> bool {
    if (!checksum) return false;
    const auto input = reinterpret_cast<std::uintptr_t>(request.data);
    const auto output = reinterpret_cast<std::uintptr_t>(checksum);
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    return request.bytes <= maximum - input && sizeof(*checksum) <= maximum - output &&
        !(input < output + sizeof(*checksum) && output < input + request.bytes);
}
[[gnu::always_inline]] inline auto canonical_dot_f64_bits(double value) noexcept -> std::uint64_t {
    auto bits = std::bit_cast<std::uint64_t>(value);
    return (bits & UINT64_C(0x7fffffffffffffff)) == 0U ? 0U : bits;
}
} // namespace epycbench::module_detail
