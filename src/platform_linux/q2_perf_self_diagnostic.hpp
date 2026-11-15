#pragma once

#include "platform_linux/perf_context_switch_access.hpp"

namespace epycbench::platform_linux::detail {

enum class Q2PerfSelfDiagnosticStatus : unsigned char {
    invalid_operations,
    open_failed,
    close_failed,
    open_closed,
};

struct Q2PerfSelfDiagnosticOutcome final {
    Q2PerfSelfDiagnosticStatus status{};
    int open_error{};
    int close_error{};
};

// The caller supplies its own native thread identity. No mapping, drain, retry,
// error allocation or descriptor transfer occurs; even descriptor zero is owned.
[[nodiscard]] auto diagnose_q2_perf_self(
    LinuxThreadId, BoundedPerfContextSwitchOperations) noexcept -> Q2PerfSelfDiagnosticOutcome;

} // namespace epycbench::platform_linux::detail

namespace epycbench::platform_linux::detail {
// Qualification only: no generic callback can issue a perf control operation.
struct PerfEpochProbeTiming final {
    void* context{};
    bool (*now)(void*, std::uint64_t&) noexcept{};
    bool (*sleep_once)(void*, std::uint64_t requested_ns) noexcept{};
};
enum class PerfEpochProbeStatus : unsigned char { failed, inconclusive, passed };
struct PerfEpochProbeOutcome final {
    PerfEpochProbeStatus status{PerfEpochProbeStatus::failed};
    const char* phase{"operations"};
    std::uint64_t elapsed_ns{}, lost_before_drain{}, lost_after_drain{};
    std::uint64_t positive_head{}, positive_tail{};
    std::size_t clean_records{}, positive_records{}, sleep_calls{};
    bool no_event_observed{}, parser_reported_lost{};
    bool disabled_idle_verified{}, clean_epoch_verified{};
    bool disable_failed{}, unmap_failed{}, close_failed{};
    int native_error{};
    BoundedPerfFirstLoss first_loss{};
};
[[nodiscard]] auto diagnose_perf_capture_epoch(
    LinuxThreadId, std::size_t page_size, BoundedPerfContextSwitchOperations,
    PerfEpochProbeTiming) noexcept -> PerfEpochProbeOutcome;
} // namespace epycbench::platform_linux::detail
