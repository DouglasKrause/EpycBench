#pragma once
#include "module_support.hpp"
#include <climits>
#include <cstddef>
#include <limits>

static_assert(CHAR_BIT == 8 && sizeof(std::uint64_t) == 8);

namespace epycbench::module_detail {
inline auto prepare_varint14(const EpycBenchKernelRequestV2* request) noexcept -> int {
    // Canonical unchanged content is a host precondition; no timed O(N) scan.
    if (!request || !request->data || !request->repetitions || request->bytes < 3U ||
        request->bytes % 3U || request->bytes > UINT64_C(3) * (UINT64_C(1) << 19U) ||
        request->bytes > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        request->repetitions > UINT64_MAX / request->bytes)
        return 1;
    // A trailing continuation marker could make the normal entry read past
    // the extent. Authentic streams end in a terminating byte.
    return (request->data[request->bytes - 1U] & 128U) != 0U ? 1 : 0;
}
inline auto valid_varint14_checksum(const EpycBenchKernelRequestV2& request,
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
