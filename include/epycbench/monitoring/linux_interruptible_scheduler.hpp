#pragma once

#include "epycbench/monitoring/acquisition_driver.hpp"

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>

namespace epycbench::monitoring {

struct detail_LinuxInterruptibleSchedulerAccess;

class LinuxInterruptibleRawMonotonicScheduler {
  public:
    LinuxInterruptibleRawMonotonicScheduler() noexcept;
    LinuxInterruptibleRawMonotonicScheduler(
        const LinuxInterruptibleRawMonotonicScheduler&) = delete;
    auto operator=(const LinuxInterruptibleRawMonotonicScheduler&)
        -> LinuxInterruptibleRawMonotonicScheduler& = delete;
    LinuxInterruptibleRawMonotonicScheduler(
        LinuxInterruptibleRawMonotonicScheduler&&) = delete;
    auto operator=(LinuxInterruptibleRawMonotonicScheduler&&)
        -> LinuxInterruptibleRawMonotonicScheduler& = delete;

    [[nodiscard]] auto wait_until(platform_linux::RawMonotonicTimestamp) noexcept
        -> std::expected<MonitoringSchedulerWaitResult, core::Error>;
    void wake() noexcept;

  private:
    struct Operations {
        void* context{};
        std::expected<platform_linux::RawMonotonicTimestamp, core::Error> (*now)(void*) noexcept{};
        void (*block)(void*, std::condition_variable&, std::unique_lock<std::mutex>&,
                      std::uint64_t) noexcept{};
    };

    explicit LinuxInterruptibleRawMonotonicScheduler(Operations, bool = false) noexcept;

    Operations operations_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool pending_wake_{};
    bool production_clock_{};

    friend struct detail_LinuxInterruptibleSchedulerAccess;
};

} // namespace epycbench::monitoring
