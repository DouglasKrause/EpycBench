#include "epycbench/platform_linux/thread_identity.hpp"

#include "platform_linux/thread_operations.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <sched.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace epycbench::platform_linux {
namespace detail {
namespace {

[[nodiscard]] auto production_get_thread_id(void*) noexcept -> RawLinuxResult {
    const auto result = gettid();
    if (result < 0) {
        const auto captured_errno = errno;
        return RawLinuxResult{.value = result, .error_number = captured_errno};
    }
    return RawLinuxResult{.value = result, .error_number = 0};
}

[[nodiscard]] auto production_get_current_cpu(void*) noexcept -> RawLinuxResult {
    const auto result = sched_getcpu();
    if (result < 0) {
        const auto captured_errno = errno;
        return RawLinuxResult{.value = result, .error_number = captured_errno};
    }
    return RawLinuxResult{.value = result, .error_number = 0};
}

[[nodiscard]] auto production_get_affinity(void*, NativeCpuMaskView destination) noexcept
    -> RawLinuxResult {
    const auto result = sched_getaffinity(0, destination.size, destination.data);
    if (result < 0) {
        const auto captured_errno = errno;
        return RawLinuxResult{.value = result, .error_number = captured_errno};
    }
    return RawLinuxResult{.value = result, .error_number = 0};
}

[[nodiscard]] auto production_set_affinity(void*, ConstNativeCpuMaskView source) noexcept
    -> RawLinuxResult {
    const auto result = sched_setaffinity(0, source.size, source.data);
    if (result < 0) {
        const auto captured_errno = errno;
        return RawLinuxResult{.value = result, .error_number = captured_errno};
    }
    return RawLinuxResult{.value = result, .error_number = 0};
}

[[nodiscard]] auto errno_classification(int error_number, std::string_view fallback)
    -> std::string_view {
    if (error_number == EINTR) {
        return "interrupted";
    }
    return fallback;
}

} // namespace

auto production_thread_operations() noexcept -> ThreadOperations {
    return ThreadOperations{
        .context = nullptr,
        .get_thread_id = production_get_thread_id,
        .get_current_cpu = production_get_current_cpu,
        .get_affinity = production_get_affinity,
        .set_affinity = production_set_affinity,
    };
}

auto thread_error(std::string_view code, std::string_view message, std::string_view operation,
                  std::string_view classification, std::optional<int> error_number,
                  std::string_view underlying_code) -> core::Error {
    auto context = nlohmann::json{{"classification", classification}};
    if (!underlying_code.empty()) {
        context["underlying_code"] = underlying_code;
    }
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(std::error_code{*error_number, std::generic_category()});
    }
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

auto contained_thread_exception(std::string_view operation) -> core::Error {
    return thread_error("linux.thread.contained_exception",
                        "Linux thread operation contained an exception", operation,
                        "contained_exception");
}

auto contained_unknown_thread_failure(std::string_view operation) -> core::Error {
    return thread_error("linux.thread.contained_unknown_failure",
                        "Linux thread operation contained an unknown failure", operation,
                        "contained_unknown_failure");
}

auto ThreadIdentityAccess::current_thread_id(const ThreadOperations& operations)
    -> std::expected<LinuxThreadId, core::Error> {
    constexpr std::string_view operation = "current_thread_id";
    try {
        const auto result = operations.get_thread_id(operations.context);
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max());
        if (result.value <= 0 || static_cast<std::uint64_t>(result.value) > maximum) {
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{thread_error(
                "linux.thread_identity.query_failed", "Linux thread ID query failed", operation,
                result.error_number == EINTR ? "interrupted" : "unexpected_thread_id_result",
                error_number)};
        }
        return LinuxThreadId::create(static_cast<std::uint64_t>(result.value));
    } catch (const std::exception&) {
        return std::unexpected{contained_thread_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_thread_failure(operation)};
    }
}

auto ThreadIdentityAccess::current_cpu(const ThreadOperations& operations)
    -> std::expected<core::LinuxCpuId, core::Error> {
    constexpr std::string_view operation = "current_cpu";
    try {
        const auto result = operations.get_current_cpu(operations.context);
        if (result.value < 0) {
            auto classification = errno_classification(result.error_number, "query_failed");
            if (result.error_number == ENOSYS) {
                classification = "current_cpu_unavailable";
            }
            const auto error_number = result.error_number == 0
                                          ? std::optional<int>{}
                                          : std::optional<int>{result.error_number};
            return std::unexpected{thread_error("linux.current_cpu.query_failed",
                                                "Current Linux CPU query failed", operation,
                                                classification, error_number)};
        }
        if (static_cast<std::uint64_t>(result.value) > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected{thread_error("linux.current_cpu.query_failed",
                                                "Current Linux CPU query was out of range",
                                                operation, "current_cpu_out_of_range")};
        }
        return core::LinuxCpuId{static_cast<std::uint32_t>(result.value)};
    } catch (const std::exception&) {
        return std::unexpected{contained_thread_exception(operation)};
    } catch (...) {
        return std::unexpected{contained_unknown_thread_failure(operation)};
    }
}

} // namespace detail

LinuxThreadId::LinuxThreadId(std::uint64_t value) noexcept : value_{value} {}

auto LinuxThreadId::create(std::uint64_t value) -> std::expected<LinuxThreadId, core::Error> {
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max());
    if (value == 0 || value > maximum) {
        return std::unexpected{detail::thread_error("linux.thread_identity.invalid_value",
                                                    "Linux thread ID value is outside its domain",
                                                    "LinuxThreadId.create", "invalid_thread_id")};
    }
    return LinuxThreadId{value};
}

auto LinuxThreadId::value() const noexcept -> std::uint64_t { return value_; }

auto current_thread_id() -> std::expected<LinuxThreadId, core::Error> {
    return detail::ThreadIdentityAccess::current_thread_id(detail::production_thread_operations());
}

auto current_cpu() -> std::expected<core::LinuxCpuId, core::Error> {
    return detail::ThreadIdentityAccess::current_cpu(detail::production_thread_operations());
}

} // namespace epycbench::platform_linux
