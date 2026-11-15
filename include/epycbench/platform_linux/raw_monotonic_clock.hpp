#pragma once

#include "epycbench/core/error.hpp"

#include <compare>
#include <cstdint>
#include <expected>

namespace epycbench::platform_linux {

namespace detail {
class RawMonotonicClockAccess;
}

class RawMonotonicTimestamp {
  public:
    [[nodiscard]] static constexpr auto from_nanoseconds(std::uint64_t nanoseconds) noexcept
        -> RawMonotonicTimestamp {
        return RawMonotonicTimestamp{nanoseconds};
    }

    [[nodiscard]] constexpr auto nanoseconds() const noexcept -> std::uint64_t {
        return nanoseconds_;
    }

    constexpr auto operator<=>(const RawMonotonicTimestamp&) const noexcept = default;

  private:
    constexpr explicit RawMonotonicTimestamp(std::uint64_t nanoseconds) noexcept
        : nanoseconds_{nanoseconds} {}

    std::uint64_t nanoseconds_;

    friend class detail::RawMonotonicClockAccess;
};

[[nodiscard]] auto raw_monotonic_now() noexcept
    -> std::expected<RawMonotonicTimestamp, core::Error>;

} // namespace epycbench::platform_linux
