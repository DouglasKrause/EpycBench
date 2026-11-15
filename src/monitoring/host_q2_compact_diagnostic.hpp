#pragma once
#include "epycbench/core/error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ2CompactOperation : std::uint8_t {
    state, create, wait, wake, join, mutex_destroy, condattr_destroy, cond_destroy,
    construction, retire, overflow, operations, occupied, clock_target, clock_raw, clock_range,
    mutex_init, mutex_lock, mutex_unlock, condattr_init, condattr_setclock, cond_init,
    cancelled, timeout, cancellation_contract, worker_count, readiness, begin, busy, generation,
    generation_missing, release, timed_missing, terminal_missing, result_missing, cancel_ready, reset
};
struct MonitoringQ2CompactDiagnostic final {
    MonitoringQ2CompactOperation operation{MonitoringQ2CompactOperation::state};
    int native_error{};
};
static_assert(std::is_trivially_copyable_v<MonitoringQ2CompactDiagnostic>);
inline auto monitoring_q2_compact_operation_name(MonitoringQ2CompactOperation value) noexcept
    -> const char* {
    switch (value) {
    case MonitoringQ2CompactOperation::cancelled: return "cancelled";
    case MonitoringQ2CompactOperation::timeout: return "timeout";
    case MonitoringQ2CompactOperation::create: return "create";
    case MonitoringQ2CompactOperation::occupied: return "occupied";
    case MonitoringQ2CompactOperation::clock_raw: return "clock_raw";
    case MonitoringQ2CompactOperation::clock_target: return "clock_target";
    case MonitoringQ2CompactOperation::clock_range: return "clock_range";
    default: return "native_cleanup";
    }
}
inline auto monitoring_q2_materialize(MonitoringQ2CompactDiagnostic value) -> core::Error {
    return {.category=core::ErrorCategory::monitoring,
      .code=core::StableErrorCode{"monitoring.q2.native_cleanup"},
      .message="Q2 native lifecycle failed",.operation="monitoring_q2_native_cleanup",
      .os_error=value.native_error==0?std::nullopt:std::optional{core::OsError{
        value.native_error,"native operation","Q2 native lifecycle"}},
      .context={{"operation",std::to_string(static_cast<std::uint8_t>(value.operation))},
                {"classification",monitoring_q2_compact_operation_name(value.operation)}},
      .severity=core::ErrorSeverity::error,
      .continuation=core::ContinuationPolicy::abort_command};
}

template<std::size_t Capacity> class MonitoringQ2CompactLedger final {
  public:
    void append(MonitoringQ2CompactDiagnostic value) noexcept {
        if (count_ < Capacity) { values_[count_++] = value; return; }
        if (!overflow_) overflow_ = true;
    }
    void note_overflow() noexcept { overflow_ = true; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return count_; }
    [[nodiscard]] auto overflowed() const noexcept -> bool { return overflow_; }
    [[nodiscard]] auto operator[](std::size_t index) const noexcept
        -> MonitoringQ2CompactDiagnostic { return values_[index]; }
  private:
    std::array<MonitoringQ2CompactDiagnostic, Capacity> values_{};
    std::size_t count_{};
    bool overflow_{};
};
} // namespace epycbench::monitoring::qualification::detail
