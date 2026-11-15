#pragma once

#include "epycbench/core/error.hpp"

#include <cstdint>
#include <expected>
#include <sys/resource.h>

namespace epycbench::platform_linux::detail {

struct ThreadRusageNativeResult {
    int native_result;
    int error_number;
    rusage usage;
};

struct ThreadRusageOperations {
    void* context;
    ThreadRusageNativeResult (*get_thread_rusage)(void*, int who);
};

struct ThreadPageFaultOracleCounters {
    std::uint64_t minor_faults;
    std::uint64_t major_faults;
    auto operator<=>(const ThreadPageFaultOracleCounters&) const noexcept = default;
};
enum class ThreadRusagePageFaultDisposition : std::uint8_t {
    succeeded,
    missing_operation,
    syscall_failed,
    unexpected_native_result,
    negative_minor_faults,
    negative_major_faults,
    counter_overflow,
    contained_standard_exception,
    contained_unknown_exception
};
enum class ThreadRusageProjectionTestFault : std::uint8_t {
    none, bad_alloc, standard_exception, unknown_exception
};
struct ThreadRusagePageFaultResult {
    ThreadRusagePageFaultDisposition disposition{};
    int error_number{};
    ThreadPageFaultOracleCounters counters{};
};

[[nodiscard]] auto production_thread_rusage_operations() noexcept -> ThreadRusageOperations;

class ThreadRusageAccess final {
  public:
    [[nodiscard]] static auto read_page_fault_counters_pod(
        const ThreadRusageOperations&) noexcept -> ThreadRusagePageFaultResult;
    [[nodiscard]] static auto project_page_fault_counters(
        ThreadRusagePageFaultResult)
        -> std::expected<ThreadPageFaultOracleCounters, core::Error>;
    static void set_projection_test_fault(ThreadRusageProjectionTestFault) noexcept;
    [[nodiscard]] static auto read_page_fault_counters(const ThreadRusageOperations&)
        -> std::expected<ThreadPageFaultOracleCounters, core::Error>;
};

} // namespace epycbench::platform_linux::detail
