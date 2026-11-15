#pragma once

#include <cstdint>
#include <optional>

namespace epycbench::kernel::detail {

[[nodiscard]] constexpr auto bounded_index(std::uint64_t draw,
                                           std::uint64_t bound) noexcept
    -> std::optional<std::uint64_t> {
    if (bound == 0U) {
        return std::nullopt;
    }
    const auto threshold = (UINT64_C(0) - bound) % bound;
    if (draw < threshold) {
        return std::nullopt;
    }
    return draw % bound;
}

} // namespace epycbench::kernel::detail
