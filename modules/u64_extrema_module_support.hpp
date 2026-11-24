#pragma once
#include "task_module_support.hpp"

namespace epycbench::module_detail {
inline auto valid_u64_extrema_checksum(const EpycBenchKernelRequestV2& request,
                                    const std::uint64_t* checksum) noexcept -> bool {
    if (!checksum) return false;
    const auto input = reinterpret_cast<std::uintptr_t>(request.data);
    const auto output = reinterpret_cast<std::uintptr_t>(checksum);
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    return request.bytes <= maximum - input && sizeof(*checksum) <= maximum - output &&
        !(input < output + sizeof(*checksum) && output < input + request.bytes);
}
} // namespace epycbench::module_detail
