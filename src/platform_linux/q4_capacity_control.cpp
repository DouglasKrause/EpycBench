#include "epycbench/platform_linux/q4_capacity_control.hpp"
#include "platform_linux/q4_capacity_control_test_access.hpp"
#include "epycbench/core/canonical_json.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <string>
#include <utility>
#include <unistd.h>
namespace epycbench::platform_linux {
namespace {
constexpr std::string_view identity="epycbench.monitoring-q4-collection-capacity/v1";
constexpr std::string_view preamble_schema="epycbench.monitoring-q4-capacity-preamble/v1";
constexpr std::string_view ack_schema="epycbench.monitoring-q4-capacity-ack/v1";
using Operations=detail::Q4CapacityControlOperations;
using Reason = Q4CapacityControlReason;
struct ControlFailure final {
    int native_error{};
    Reason reason{Reason::none};
};
auto wait_fd(int fd, short events, std::chrono::steady_clock::time_point deadline,
             const Operations& operations) -> ControlFailure {
    const bool writing = events == POLLOUT;
    for (;;) {
        const auto now = operations.now();
        if (now >= deadline)
            return {ETIMEDOUT, writing ? Reason::write_deadline : Reason::read_deadline};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
        pollfd item{fd, events, 0};
        const auto result = operations.poll(&item, 1, static_cast<int>(std::min<std::int64_t>(remaining+1, INT_MAX)));
        if (result > 0) {
            if ((item.revents & events) != 0 || (events == POLLIN && (item.revents & POLLHUP) != 0))
                return {};
            return {EIO, writing ? Reason::write_events : Reason::read_events};
        }
        if (result == 0)
            return {ETIMEDOUT, writing ? Reason::write_poll_timeout : Reason::read_poll_timeout};
        if (errno != EINTR)
            return {errno, writing ? Reason::write_poll_error : Reason::read_poll_error};
    }
}
auto write_all(int fd, std::string_view bytes, std::chrono::steady_clock::time_point deadline,
               const Operations& operations) -> ControlFailure {
    while (!bytes.empty()) {
        if (const auto failure = wait_fd(fd, POLLOUT, deadline, operations); failure.native_error)
            return failure;
        iovec vector{const_cast<char*>(bytes.data()), bytes.size()};
        msghdr message{}; message.msg_iov = &vector; message.msg_iovlen = 1;
        const auto count = operations.sendmsg(fd, &message, MSG_NOSIGNAL);
        if (count > 0) { bytes.remove_prefix(static_cast<std::size_t>(count)); continue; }
        if (count < 0 && errno == EINTR) continue;
        return count < 0 ? ControlFailure{errno, Reason::write_send}
                         : ControlFailure{EIO, Reason::write_zero};
    }
    return {};
}
auto read_line(int fd, std::chrono::steady_clock::time_point deadline,
               const Operations& operations) -> std::pair<std::string, ControlFailure> {
    std::string out; out.reserve(256); bool complete = false;
    for (;;) {
        if (const auto failure = wait_fd(fd, POLLIN, deadline, operations); failure.native_error)
            return {{}, failure};
        char value{};
        const auto count = operations.read(fd, &value, 1);
        if (count == 1) {
            if (complete) return {{}, {EPROTO, Reason::read_trailing}};
            out.push_back(value);
            if (out.size() > 256U) return {{}, {EMSGSIZE, Reason::read_limit}};
            if (value == '\n') complete = true;
            continue;
        }
        if (count == 0) {
            if (complete) return {std::move(out), {}};
            return {{}, {EPIPE, Reason::read_premature_eof}};
        }
        if (errno == EINTR) continue;
        return {{}, {errno, Reason::read_error}};
    }
}
auto shutdown_write(int fd, std::chrono::steady_clock::time_point deadline,
                    const Operations& operations) -> ControlFailure {
    for (;;) {
        if (operations.now() >= deadline) return {ETIMEDOUT, Reason::shutdown_deadline};
        if (operations.shutdown(fd, SHUT_WR) == 0) return {};
        if (errno != EINTR) return {errno, Reason::shutdown_error};
    }
}
auto exchange_with_operations(int descriptor, std::uint64_t evidence_bytes, std::uint64_t stdout_bytes,
                              std::uint64_t timeout_milliseconds, const Operations& operations) noexcept
    -> Q4CapacityControlResult {
    const auto finish = [&](Q4CapacityControlResult result) noexcept {
        result.primary_failure = !result.acknowledged;
        if (descriptor >= 0 && operations.close(std::exchange(descriptor, -1)) != 0) {
            result.cleanup_native_error = errno;
            result.close_failed = true;
            result.acknowledged = false;
        }
        return result;
    };
    const auto fail = [&](Reason reason, int native = 0) noexcept {
        return finish({false, native, 0, false, true, reason});
    };
    auto exception_scope = Reason::preamble_exception_scope;
    try {
        if (descriptor < 0) return fail(Reason::descriptor);
        if (evidence_bytes == 0) return fail(Reason::evidence_zero);
        if (stdout_bytes < evidence_bytes) return fail(Reason::stdout_short);
        if (timeout_milliseconds == 0) return fail(Reason::timeout_zero);
        const auto pre = core::canonicalize_json(nlohmann::ordered_json{{"schema",preamble_schema},{"capacity_identity",identity},{"evidence_bytes",evidence_bytes},{"stdout_bytes",stdout_bytes}});
        exception_scope = Reason::ack_exception_scope;
        const auto expected = core::canonicalize_json(nlohmann::ordered_json{{"schema",ack_schema},{"capacity_identity",identity},{"evidence_bytes",evidence_bytes},{"stdout_bytes",stdout_bytes}});
        if (!pre) return fail(Reason::preamble_canonical);
        if (!expected) return fail(Reason::ack_canonical);
        exception_scope = Reason::wire_exception_scope;
        const auto deadline = operations.now() + std::chrono::milliseconds(timeout_milliseconds);
        const auto wire = *pre + "\n";
        if (wire.size() > 256U) return fail(Reason::preamble_limit);
        exception_scope = Reason::write_exception_scope;
        if (const auto failure = write_all(descriptor, wire, deadline, operations); failure.native_error)
            return fail(failure.reason, failure.native_error);
        exception_scope = Reason::shutdown_exception_scope;
        if (const auto failure = shutdown_write(descriptor, deadline, operations); failure.native_error)
            return fail(failure.reason, failure.native_error);
        exception_scope = Reason::read_exception_scope;
        auto [ack, failure] = read_line(descriptor, deadline, operations);
        if (failure.native_error) return fail(failure.reason, failure.native_error);
        exception_scope = Reason::compare_exception_scope;
        const bool acknowledged = ack == *expected + "\n";
        if (!acknowledged) return fail(Reason::ack_mismatch, EPROTO);
        return finish({true});
    } catch (...) {
        return fail(exception_scope);
    }
}
}
auto q4_capacity_control_exchange(int descriptor,std::uint64_t evidence_bytes,std::uint64_t stdout_bytes,
                                  std::uint64_t timeout_milliseconds) noexcept->Q4CapacityControlResult{
 const Operations operations{::poll,::sendmsg,::read,::shutdown,::close,
                             +[]{return std::chrono::steady_clock::now();}};
 return exchange_with_operations(descriptor,evidence_bytes,stdout_bytes,timeout_milliseconds,operations);
}
auto detail::q4_capacity_control_exchange_with_operations(int descriptor,std::uint64_t evidence_bytes,
 std::uint64_t stdout_bytes,std::uint64_t timeout_milliseconds,const Operations& operations) noexcept->Q4CapacityControlResult{
 return exchange_with_operations(descriptor,evidence_bytes,stdout_bytes,timeout_milliseconds,operations);
}
}
