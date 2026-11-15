#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/id.hpp"

#include <compare>
#include <cstdint>
#include <expected>

namespace epycbench::platform_linux {

namespace detail {
class ThreadIdentityAccess;
}

class LinuxThreadId {
  public:
    [[nodiscard]] static auto create(std::uint64_t value)
        -> std::expected<LinuxThreadId, core::Error>;

    [[nodiscard]] auto value() const noexcept -> std::uint64_t;

    auto operator<=>(const LinuxThreadId&) const noexcept = default;

  private:
    explicit LinuxThreadId(std::uint64_t value) noexcept;

    std::uint64_t value_;

    friend class detail::ThreadIdentityAccess;
};

[[nodiscard]] auto current_thread_id() -> std::expected<LinuxThreadId, core::Error>;

[[nodiscard]] auto current_cpu() -> std::expected<core::LinuxCpuId, core::Error>;

} // namespace epycbench::platform_linux
