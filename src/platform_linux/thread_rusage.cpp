#include "platform_linux/thread_rusage_operations.hpp"

#include <cerrno>
#include <exception>
#include <optional>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace epycbench::platform_linux::detail {
namespace {
thread_local ThreadRusageProjectionTestFault projection_test_fault{};

auto production_getrusage(void*, int who) -> ThreadRusageNativeResult {
    rusage usage{};
    const auto result = ::getrusage(who, &usage);
    return {.native_result = result,
            .error_number = result == -1 ? errno : 0,
            .usage = usage};
}

auto oracle_error(std::string_view code, std::string_view classification,
                  std::optional<int> error_number = std::nullopt) -> core::Error {
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0)
        os_error = core::OsError::capture(std::error_code{*error_number,
                                                          std::generic_category()});
    return {.category = core::ErrorCategory::platform_linux,
            .code = core::StableErrorCode{std::string{code}},
            .message = "Thread page-fault oracle acquisition failed",
            .operation = "thread_rusage_page_faults",
            .os_error = std::move(os_error),
            .context = {{"classification", classification}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

} // namespace

auto production_thread_rusage_operations() noexcept -> ThreadRusageOperations {
    return {.context = nullptr, .get_thread_rusage = production_getrusage};
}

auto ThreadRusageAccess::read_page_fault_counters(const ThreadRusageOperations& operations)
    -> std::expected<ThreadPageFaultOracleCounters, core::Error> {
    return project_page_fault_counters(read_page_fault_counters_pod(operations));
}

auto ThreadRusageAccess::read_page_fault_counters_pod(
    const ThreadRusageOperations& operations) noexcept -> ThreadRusagePageFaultResult {
    try {
        if (operations.get_thread_rusage == nullptr)
            return {.disposition = ThreadRusagePageFaultDisposition::missing_operation};
        const auto result = operations.get_thread_rusage(operations.context, RUSAGE_THREAD);
        if (result.native_result == -1 && result.error_number > 0)
            return {.disposition = ThreadRusagePageFaultDisposition::syscall_failed,
                    .error_number = result.error_number};
        if (result.native_result != 0 || result.error_number != 0)
            return {.disposition = ThreadRusagePageFaultDisposition::unexpected_native_result};
        if constexpr (std::is_signed_v<decltype(result.usage.ru_minflt)>) {
            if (result.usage.ru_minflt < 0)
                return {.disposition = ThreadRusagePageFaultDisposition::negative_minor_faults};
            if (result.usage.ru_majflt < 0)
                return {.disposition = ThreadRusagePageFaultDisposition::negative_major_faults};
        }
        if (!std::in_range<std::uint64_t>(result.usage.ru_minflt) ||
            !std::in_range<std::uint64_t>(result.usage.ru_majflt))
            return {.disposition = ThreadRusagePageFaultDisposition::counter_overflow};
        return {.disposition = ThreadRusagePageFaultDisposition::succeeded,
                .counters = {.minor_faults = static_cast<std::uint64_t>(result.usage.ru_minflt),
                             .major_faults = static_cast<std::uint64_t>(result.usage.ru_majflt)}};
    } catch (const std::exception&) {
        return {.disposition = ThreadRusagePageFaultDisposition::contained_standard_exception};
    } catch (...) {
        return {.disposition = ThreadRusagePageFaultDisposition::contained_unknown_exception};
    }
}

auto ThreadRusageAccess::project_page_fault_counters(ThreadRusagePageFaultResult result)
    -> std::expected<ThreadPageFaultOracleCounters, core::Error> {
    using D = ThreadRusagePageFaultDisposition;
    const auto fault = std::exchange(projection_test_fault,
                                     ThreadRusageProjectionTestFault::none);
    if (fault == ThreadRusageProjectionTestFault::bad_alloc) throw std::bad_alloc{};
    if (fault == ThreadRusageProjectionTestFault::standard_exception)
        throw std::runtime_error{"projection"};
    if (fault == ThreadRusageProjectionTestFault::unknown_exception) throw 7;
    switch (result.disposition) {
    case D::succeeded: return result.counters;
    case D::missing_operation:
        return std::unexpected{oracle_error("linux.thread_rusage.invalid_operations",
                                            "missing_getrusage")};
    case D::syscall_failed:
        return std::unexpected{oracle_error("linux.thread_rusage.read_failed", "syscall_failed",
                                            result.error_number)};
    case D::unexpected_native_result:
        return std::unexpected{oracle_error("linux.thread_rusage.read_failed",
                                            "unexpected_native_result")};
    case D::negative_minor_faults:
        return std::unexpected{oracle_error("linux.thread_rusage.invalid_counter",
                                            "negative_minor_faults")};
    case D::negative_major_faults:
        return std::unexpected{oracle_error("linux.thread_rusage.invalid_counter",
                                            "negative_major_faults")};
    case D::counter_overflow:
        return std::unexpected{oracle_error("linux.thread_rusage.invalid_counter",
                                            "counter_overflow")};
    case D::contained_standard_exception:
        return std::unexpected{oracle_error("linux.thread_rusage.contained_exception",
                                            "contained_standard_exception")};
    case D::contained_unknown_exception:
        return std::unexpected{oracle_error("linux.thread_rusage.contained_unknown_failure",
                                            "contained_unknown_exception")};
    }
    return std::unexpected{oracle_error("linux.thread_rusage.contained_unknown_failure",
                                        "contained_unknown_exception")};
}

void ThreadRusageAccess::set_projection_test_fault(
    ThreadRusageProjectionTestFault fault) noexcept {
    projection_test_fault = fault;
}

} // namespace epycbench::platform_linux::detail
