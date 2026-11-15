#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/platform_linux/thread_identity.hpp"

#include <compare>
#include <cstdint>
#include <expected>

namespace epycbench::platform_linux {

namespace detail {
class TaskStatAccess;
}

class LinuxTaskPageFaultCounters {
  public:
    [[nodiscard]] static constexpr auto create(std::uint64_t minor_faults,
                                               std::uint64_t major_faults) noexcept
        -> LinuxTaskPageFaultCounters {
        return LinuxTaskPageFaultCounters{minor_faults, major_faults};
    }

    [[nodiscard]] constexpr auto minor_faults() const noexcept -> std::uint64_t {
        return minor_faults_;
    }
    [[nodiscard]] constexpr auto major_faults() const noexcept -> std::uint64_t {
        return major_faults_;
    }

    constexpr auto operator<=>(const LinuxTaskPageFaultCounters&) const noexcept = default;

  private:
    constexpr LinuxTaskPageFaultCounters(std::uint64_t minor_faults,
                                         std::uint64_t major_faults) noexcept
        : minor_faults_{minor_faults}, major_faults_{major_faults} {}

    std::uint64_t minor_faults_;
    std::uint64_t major_faults_;

    friend class detail::TaskStatAccess;
};

[[nodiscard]] auto read_task_page_fault_counters(LinuxThreadId thread_id) noexcept
    -> std::expected<LinuxTaskPageFaultCounters, core::Error>;

} // namespace epycbench::platform_linux
