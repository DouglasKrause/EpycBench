#include "epycbench/platform_linux/raw_monotonic_clock.hpp"

#include "platform_linux/clock_operations.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <time.h>
#include <type_traits>
#include <utility>

namespace epycbench::platform_linux {
namespace detail {
namespace {

constexpr std::uint64_t nanoseconds_per_second{1'000'000'000U};

[[nodiscard]] auto production_clock_gettime(void*, clockid_t clock_id) noexcept
    -> RawClockResult {
    timespec timestamp{};
    const auto native_result = ::clock_gettime(clock_id, &timestamp);
    if (native_result == -1) {
        const auto captured_errno = errno;
        return {.native_result = native_result,
                .error_number = captured_errno,
                .timestamp = timestamp};
    }
    return {.native_result = native_result, .error_number = 0, .timestamp = timestamp};
}

[[nodiscard]] auto clock_error(std::string_view code, std::string_view message,
                               std::string_view phase, std::string_view classification,
                               std::optional<int> error_number = std::nullopt) -> core::Error {
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(
            std::error_code{*error_number, std::generic_category()});
    }
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = "raw_monotonic_now",
        .os_error = std::move(os_error),
        .context = nlohmann::json{{"clock_profile", "monotonic-raw"},
                                  {"phase", phase},
                                  {"classification", classification}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto contained_exception() -> core::Error {
    return clock_error("linux.raw_monotonic_clock.contained_exception",
                       "Raw monotonic clock operation contained an exception", "read",
                       "contained_exception");
}

[[nodiscard]] auto contained_unknown_failure() -> core::Error {
    return clock_error("linux.raw_monotonic_clock.contained_unknown_failure",
                       "Raw monotonic clock operation contained an unknown failure", "read",
                       "contained_unknown_failure");
}

[[nodiscard]] auto invalid_native_result() -> core::Error {
    return clock_error("linux.raw_monotonic_clock.read_failed",
                       "Raw monotonic clock returned inconsistent native status", "read",
                       "unexpected_native_result");
}

} // namespace

auto production_clock_operations() noexcept -> ClockOperations {
    return ClockOperations{.context = nullptr, .clock_gettime_raw = production_clock_gettime};
}

auto RawMonotonicClockAccess::now(const ClockOperations& operations) noexcept
    -> std::expected<RawMonotonicTimestamp, core::Error> {
    try {
        const auto result = operations.clock_gettime_raw(operations.context, CLOCK_MONOTONIC_RAW);
        if (result.native_result == -1 && result.error_number != 0) {
            const auto classification = result.error_number == EINTR
                                            ? "interrupted"
                                            : result.error_number == EINVAL ? "clock_unavailable"
                                                                            : "read_failed";
            return std::unexpected{clock_error(
                "linux.raw_monotonic_clock.read_failed", "Raw monotonic clock read failed",
                "read", classification, result.error_number)};
        }
        if (result.native_result != 0 || result.error_number != 0) {
            return std::unexpected{invalid_native_result()};
        }

        if constexpr (std::is_signed_v<time_t>) {
            if (result.timestamp.tv_sec < 0) {
                return std::unexpected{clock_error(
                    "linux.raw_monotonic_clock.invalid_timestamp",
                    "Raw monotonic clock returned an invalid timestamp", "validate",
                    "negative_seconds")};
            }
        }
        if (result.timestamp.tv_nsec < 0 ||
            result.timestamp.tv_nsec >= static_cast<decltype(result.timestamp.tv_nsec)>(
                                                nanoseconds_per_second)) {
            return std::unexpected{clock_error(
                "linux.raw_monotonic_clock.invalid_timestamp",
                "Raw monotonic clock returned an invalid timestamp", "validate",
                "nanoseconds_out_of_range")};
        }
        if (!std::in_range<std::uint64_t>(result.timestamp.tv_sec)) {
            return std::unexpected{clock_error(
                "linux.raw_monotonic_clock.conversion_failed",
                "Raw monotonic timestamp conversion overflowed", "convert",
                "timestamp_overflow")};
        }

        const auto seconds = static_cast<std::uint64_t>(result.timestamp.tv_sec);
        if (seconds > std::numeric_limits<std::uint64_t>::max() / nanoseconds_per_second) {
            return std::unexpected{clock_error(
                "linux.raw_monotonic_clock.conversion_failed",
                "Raw monotonic timestamp conversion overflowed", "convert",
                "timestamp_overflow")};
        }
        const auto seconds_nanoseconds = seconds * nanoseconds_per_second;
        const auto nanoseconds = static_cast<std::uint64_t>(result.timestamp.tv_nsec);
        if (nanoseconds >
            std::numeric_limits<std::uint64_t>::max() - seconds_nanoseconds) {
            return std::unexpected{clock_error(
                "linux.raw_monotonic_clock.conversion_failed",
                "Raw monotonic timestamp conversion overflowed", "convert",
                "timestamp_overflow")};
        }
        return RawMonotonicTimestamp{seconds_nanoseconds + nanoseconds};
    } catch (const std::exception&) {
        return std::unexpected{contained_exception()};
    } catch (...) {
        return std::unexpected{contained_unknown_failure()};
    }
}

} // namespace detail

auto raw_monotonic_now() noexcept -> std::expected<RawMonotonicTimestamp, core::Error> {
    return detail::RawMonotonicClockAccess::now(detail::production_clock_operations());
}

} // namespace epycbench::platform_linux
