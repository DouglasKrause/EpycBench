#pragma once

#include "module_support.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace epycbench::module_detail {

inline auto prepare_sum_u64(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (!request || !request->data || request->bytes == 0U ||
        request->bytes % sizeof(std::uint64_t) != 0U || request->repetitions == 0U ||
        request->bytes > std::numeric_limits<std::size_t>::max()) return 1;
    const auto count = request->bytes / sizeof(std::uint64_t);
    return request->repetitions <= std::numeric_limits<std::uint64_t>::max() / count
        ? 0 : 1;
}

[[gnu::always_inline]] inline auto load_sum_word(
    const unsigned char* input, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t value;
    std::memcpy(&value, input + index * sizeof(value), sizeof(value));
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return value;
}

} // namespace epycbench::module_detail
