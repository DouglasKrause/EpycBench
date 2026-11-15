#pragma once
#include <cstdint>
#include <limits>
#include <optional>
namespace epycbench::results::journal::detail {
// Arithmetic seam only: no owner can resume an externally selected chain state.
[[nodiscard]] constexpr auto next_sequence(std::uint64_t count) noexcept
    -> std::optional<std::uint64_t> {
    if (count == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return count + 1;
}
} // namespace epycbench::results::journal::detail
