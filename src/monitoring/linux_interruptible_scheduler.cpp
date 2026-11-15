#include "epycbench/monitoring/linux_interruptible_scheduler.hpp"

#include "epycbench/platform_linux/raw_monotonic_clock.hpp"
#include "interruptible_raw_scheduler_kernel.hpp"

#include <chrono>
#include <limits>
#include <utility>

namespace epycbench::monitoring {
namespace {

auto production_now(void*) noexcept
    -> std::expected<platform_linux::RawMonotonicTimestamp, core::Error> {
    return platform_linux::raw_monotonic_now();
}

void production_block(void*, std::condition_variable& condition,
                      std::unique_lock<std::mutex>& lock,
                      std::uint64_t nanoseconds) noexcept {
    using Rep = std::chrono::nanoseconds::rep;
    condition.wait_for(lock, std::chrono::nanoseconds{static_cast<Rep>(nanoseconds)});
}

} // namespace

LinuxInterruptibleRawMonotonicScheduler::LinuxInterruptibleRawMonotonicScheduler() noexcept
    : LinuxInterruptibleRawMonotonicScheduler{
          Operations{nullptr, production_now, production_block}, true} {}

LinuxInterruptibleRawMonotonicScheduler::LinuxInterruptibleRawMonotonicScheduler(
    Operations operations, bool production_clock) noexcept
    : operations_{operations}, production_clock_{production_clock} {}

auto LinuxInterruptibleRawMonotonicScheduler::wait_until(
    platform_linux::RawMonotonicTimestamp deadline) noexcept
    -> std::expected<MonitoringSchedulerWaitResult, core::Error> {
    auto waited = detail::wait_until_raw_monotonic(
        mutex_, condition_, pending_wake_, deadline, [&]() noexcept {
            return operations_.now(operations_.context);
        }, [&](std::condition_variable& condition,
               std::unique_lock<std::mutex>& lock,
               std::uint64_t nanoseconds) noexcept {
            operations_.block(operations_.context, condition, lock, nanoseconds);
        });
    if (!waited)
        return std::unexpected{std::move(waited.error())};
    return *waited == detail::InterruptibleRawWaitResult::deadline
        ? MonitoringSchedulerWaitResult::deadline
        : MonitoringSchedulerWaitResult::interrupted;
}

void LinuxInterruptibleRawMonotonicScheduler::wake() noexcept {
    detail::wake_raw_monotonic(mutex_, condition_, pending_wake_);
}

} // namespace epycbench::monitoring
