#pragma once
#include "epycbench/core/error.hpp"
#include "monitoring/host_q2_compact_diagnostic.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <pthread.h>

namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ2CancellationUnregister : std::uint8_t {
    quiescent,
    failed,
};
struct MonitoringQ2WaitCancellation final {
    void* context{};
    bool (*requested)(void*) noexcept {};
    void (*register_wake)(void*, void*, void (*)(void*) noexcept) noexcept {};
    MonitoringQ2CancellationUnregister (*unregister_wake)(void*, void*) noexcept {};
};
struct MonitoringQ2RawDeadline final {
    std::uint64_t nanoseconds{};
};
struct MonitoringQ2DeadlineCancellationContext final {
    MonitoringQ2RawDeadline deadline;
    MonitoringQ2WaitCancellation cancellation;
};
class MonitoringQ2WaitContextCapsule final {
  public:
    void publish(MonitoringQ2DeadlineCancellationContext value) noexcept {
        sequence_.fetch_add(1U, std::memory_order_acq_rel);
        deadline_.store(value.deadline.nanoseconds, std::memory_order_relaxed);
        context_.store(value.cancellation.context, std::memory_order_relaxed);
        requested_.store(value.cancellation.requested, std::memory_order_relaxed);
        register_.store(value.cancellation.register_wake, std::memory_order_relaxed);
        unregister_.store(value.cancellation.unregister_wake, std::memory_order_relaxed);
        sequence_.fetch_add(1U, std::memory_order_release);
    }
    auto load() const noexcept -> MonitoringQ2DeadlineCancellationContext {
        for (;;) {
            const auto before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0U)
                continue;
            MonitoringQ2DeadlineCancellationContext result{
                {deadline_.load(std::memory_order_relaxed)},
                {context_.load(std::memory_order_relaxed),
                 requested_.load(std::memory_order_relaxed),
                 register_.load(std::memory_order_relaxed),
                 unregister_.load(std::memory_order_relaxed)}};
            if (sequence_.load(std::memory_order_acquire) == before)
                return result;
        }
    }

  private:
    std::atomic<std::uint64_t> sequence_{}, deadline_{};
    std::atomic<void*> context_{};
    std::atomic<bool (*)(void*) noexcept> requested_{};
    std::atomic<void (*)(void*, void*, void (*)(void*) noexcept) noexcept> register_{};
    std::atomic<MonitoringQ2CancellationUnregister (*)(void*, void*) noexcept> unregister_{};
};
enum class MonitoringQ2NativeThreadRole : std::uint8_t { target, helper };
struct MonitoringQ2NativeThreadSlot final {
    MonitoringQ2NativeThreadRole role{MonitoringQ2NativeThreadRole::helper};
    std::size_t target_index{};
};
struct MonitoringQ2NativeThreadSystem final {
    void* context{};
    int (*clock_gettime)(void*, clockid_t, timespec*) noexcept {};
    int (*mutex_init)(void*, pthread_mutex_t*, const pthread_mutexattr_t*) noexcept {};
    int (*mutex_destroy)(void*, pthread_mutex_t*) noexcept {};
    int (*mutex_lock)(void*, pthread_mutex_t*) noexcept {};
    int (*mutex_unlock)(void*, pthread_mutex_t*) noexcept {};
    int (*condattr_init)(void*, pthread_condattr_t*) noexcept {};
    int (*condattr_setclock)(void*, pthread_condattr_t*, clockid_t) noexcept {};
    int (*condattr_destroy)(void*, pthread_condattr_t*) noexcept {};
    int (*cond_init)(void*, pthread_cond_t*, const pthread_condattr_t*) noexcept {};
    int (*cond_destroy)(void*, pthread_cond_t*) noexcept {};
    int (*cond_broadcast)(void*, pthread_cond_t*) noexcept {};
    int (*cond_timedwait)(void*, pthread_cond_t*, pthread_mutex_t*, const timespec*) noexcept {};
    int (*thread_create)(void*, pthread_t*, const pthread_attr_t*, void* (*)(void*),
                         void*) noexcept {};
    int (*thread_timedjoin)(void*, pthread_t, void**, const timespec*) noexcept {};
    int (*thread_clockjoin)(void*, pthread_t, void**, clockid_t, const timespec*) noexcept {};
};
class MonitoringQ2NativeThreadCreateOutcome;
class MonitoringQ2NativeThreadOwner final {
  public:
    class StopToken final {
      public:
        explicit StopToken(const std::atomic<bool>* value) noexcept : value_{value} {}
        auto stop_requested() const noexcept -> bool {
            return value_->load(std::memory_order_acquire);
        }

      private:
        const std::atomic<bool>* value_{};
    };
    using Entry = void (*)(void*, StopToken) noexcept;
    using Prepare = std::expected<void, core::Error> (*)(void*) noexcept;
    static constexpr std::size_t maximum_targets = 512;
    // Attributes are borrowed only during creation and must describe a joinable
    // thread. Caller-supplied stack backing stays live through confirmed retirement.
    static auto create_parked(MonitoringQ2NativeThreadSlot, void*, Entry,
                              MonitoringQ2DeadlineCancellationContext,
                              const MonitoringQ2NativeThreadSystem* = nullptr,
                              Prepare = nullptr, const pthread_attr_t* = nullptr,
                              std::uint64_t cancellation_poll_ns = 0U) noexcept
        -> MonitoringQ2NativeThreadCreateOutcome;
    static auto create_parked(void*, Entry, MonitoringQ2DeadlineCancellationContext) noexcept
        -> MonitoringQ2NativeThreadCreateOutcome;
    MonitoringQ2NativeThreadOwner(MonitoringQ2NativeThreadOwner&&) noexcept;
    auto operator=(MonitoringQ2NativeThreadOwner&&) noexcept -> MonitoringQ2NativeThreadOwner&;
    MonitoringQ2NativeThreadOwner(const MonitoringQ2NativeThreadOwner&) = delete;
    ~MonitoringQ2NativeThreadOwner() noexcept;
    auto release_start() noexcept -> std::expected<void, MonitoringQ2CompactDiagnostic>;
    void request_stop() noexcept;
    auto timed_join(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, MonitoringQ2CompactDiagnostic>;
    auto take_cleanup_diagnostics() noexcept -> MonitoringQ2CompactLedger<2>;
    auto native_handle() const noexcept -> pthread_t;
    auto active() const noexcept -> bool;
    auto quarantined() const noexcept -> bool;
    auto slot() const noexcept -> MonitoringQ2NativeThreadSlot;
    auto token() const noexcept -> std::uint64_t;

  private:
    struct State;
    explicit MonitoringQ2NativeThreadOwner(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class MonitoringQ2NativeThreadCreateOutcome;
};
struct MonitoringQ2NativeThreadCreateFailure final {
    explicit MonitoringQ2NativeThreadCreateFailure(MonitoringQ2CompactDiagnostic value) noexcept
        : primary{value} {}
    MonitoringQ2CompactDiagnostic primary;
    MonitoringQ2CompactLedger<3> cleanup;
    std::optional<core::Error> authored_primary;
};
class MonitoringQ2NativeThreadCreateOutcome final {
  public:
    MonitoringQ2NativeThreadCreateOutcome(MonitoringQ2NativeThreadCreateOutcome&&) noexcept =
        default;
    MonitoringQ2NativeThreadCreateOutcome(const MonitoringQ2NativeThreadCreateOutcome&) = delete;
    explicit operator bool() const noexcept { return owner_.has_value() && !failure_; }
    auto owner() && noexcept -> MonitoringQ2NativeThreadOwner { return std::move(*owner_); }
    auto error() const noexcept -> const MonitoringQ2NativeThreadCreateFailure& {
        return *failure_;
    }
    auto take_failure() && noexcept -> MonitoringQ2NativeThreadCreateFailure {
        return std::move(*failure_);
    }
    auto recovery_owner() && noexcept -> std::optional<MonitoringQ2NativeThreadOwner> {
        return std::move(owner_);
    }

  private:
    MonitoringQ2NativeThreadCreateOutcome(
        std::optional<MonitoringQ2NativeThreadOwner> owner,
        std::optional<MonitoringQ2NativeThreadCreateFailure> failure)
        : owner_{std::move(owner)}, failure_{std::move(failure)} {}
    std::optional<MonitoringQ2NativeThreadOwner> owner_;
    std::optional<MonitoringQ2NativeThreadCreateFailure> failure_;
    friend class MonitoringQ2NativeThreadOwner;
};
// Same bounded 32-bit native wait, with nonallocating failure transport.
auto monitoring_q2_bounded_atomic_wait_compact(const std::atomic<std::uint32_t>&, std::uint32_t,
                                               MonitoringQ2DeadlineCancellationContext) noexcept
    -> std::expected<void, MonitoringQ2CompactDiagnostic>;
auto monitoring_q2_bounded_atomic_wait(const std::atomic<std::uint32_t>&, std::uint32_t,
                                       MonitoringQ2DeadlineCancellationContext) noexcept
    -> std::expected<void, core::Error>;
auto monitoring_q2_bounded_atomic_wait(const std::atomic<std::uint64_t>&, std::uint64_t,
                                       MonitoringQ2DeadlineCancellationContext) noexcept
    -> std::expected<void, core::Error>;
void monitoring_q2_atomic_wake(std::atomic<std::uint32_t>&) noexcept;
void monitoring_q2_atomic_wake(std::atomic<std::uint64_t>&) noexcept;
} // namespace epycbench::monitoring::qualification::detail
