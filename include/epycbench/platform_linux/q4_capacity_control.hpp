#pragma once
#include <cstdint>
namespace epycbench::platform_linux {
enum class Q4CapacityControlReason : std::uint16_t {
    none,
    unspecified,
    ack_exception_scope,
    ack_canonical,
    ack_mismatch,
    compare_exception_scope,
    descriptor,
    evidence_zero,
    preamble_exception_scope,
    preamble_canonical,
    preamble_limit,
    read_deadline,
    read_error,
    read_events,
    read_exception_scope,
    read_limit,
    read_poll_error,
    read_poll_timeout,
    read_premature_eof,
    read_trailing,
    shutdown_deadline,
    shutdown_error,
    shutdown_exception_scope,
    stdout_short,
    timeout_zero,
    wire_exception_scope,
    write_deadline,
    write_events,
    write_exception_scope,
    write_poll_error,
    write_poll_timeout,
    write_send,
    write_zero,
};
struct Q4CapacityControlResult final { bool acknowledged{}; int native_error{}; int cleanup_native_error{}; bool close_failed{}; bool primary_failure{}; Q4CapacityControlReason reason{Q4CapacityControlReason::none}; };
// Takes ownership of descriptor and retires it exactly once on every return path.
[[nodiscard]] auto q4_capacity_control_exchange(int descriptor,std::uint64_t evidence_bytes,
                                                 std::uint64_t stdout_bytes,
                                                 std::uint64_t timeout_milliseconds) noexcept
    -> Q4CapacityControlResult;
}
