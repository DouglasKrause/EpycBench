#pragma once
#include "module_support.hpp"
#include <bit>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
static_assert(CHAR_BIT == 8 && sizeof(std::uint64_t) == 8 &&
              std::endian::native == std::endian::little);
namespace epycbench::module_detail {
inline auto prepare_chase4_u64(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (!request || !request->data || !request->repetitions || request->bytes < 256U ||
        request->bytes % 256U ||
        request->bytes > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        reinterpret_cast<std::uintptr_t>(request->data) % 64U)
        return 1;
    const auto loads = 4U * (request->bytes / 256U + 1U);
    return request->repetitions <= UINT64_MAX / loads ? 0 : 1;
}
inline auto valid_chase4_u64_checksum(const EpycBenchKernelRequestV2& request,
                                      const std::uint64_t* checksum) noexcept -> bool {
    if (!checksum)
        return false;
    const auto input = reinterpret_cast<std::uintptr_t>(request.data),
               output = reinterpret_cast<std::uintptr_t>(checksum);
    if (input > UINTPTR_MAX - request.bytes || output > UINTPTR_MAX - sizeof(*checksum))
        return false;
    return output + sizeof(*checksum) <= input || input + request.bytes <= output;
}
} // namespace epycbench::module_detail
