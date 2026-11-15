#include "epycbench/platform_linux/local_ipc.hpp"

#include "platform_linux/process_ipc_operations.hpp"

#include <array>
#include <cerrno>
#include <exception>
#include <limits>
#include <optional>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace epycbench::platform_linux {
namespace detail {
namespace {

[[nodiscard]] auto ipc_error(std::string_view code, std::string_view message,
                             std::string_view operation, std::string_view classification,
                             std::optional<int> error_number = std::nullopt)
    -> core::Error {
    std::optional<core::OsError> os_error;
    if (error_number && *error_number != 0) {
        os_error = core::OsError::capture(
            std::error_code{*error_number, std::generic_category()});
    }
    return core::Error{
        .category = core::ErrorCategory::platform_linux,
        .code = core::StableErrorCode{std::string{code}},
        .message = std::string{message},
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = nlohmann::json{{"classification", classification}},
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto io_classification(int error_number, bool writing) -> std::string_view {
    if (error_number == EINTR) return "interrupted";
    if (error_number == EAGAIN || error_number == EWOULDBLOCK) return "would_block";
    if (error_number == ECONNRESET) return "connection_reset";
    if (writing && error_number == EPIPE) return "broken_pipe";
    return "native_failure";
}

void cleanup_raw(const DescriptorOperations& operations, int descriptor) noexcept {
    if (descriptor >= 0) {
        static_cast<void>(operations.close(operations.context, descriptor));
    }
}

[[nodiscard]] auto normalize(FileDescriptor descriptor,
                             const DescriptorOperations& operations)
    -> std::expected<FileDescriptor, core::Error> {
    if (descriptor.native_handle() >= 5) {
        return descriptor;
    }
    auto duplicated = DescriptorAccess::duplicate_at_least(descriptor, 5, operations);
    if (!duplicated) {
        return std::unexpected{std::move(duplicated.error())};
    }
    auto closed = descriptor.close();
    if (!closed) {
        return std::unexpected{std::move(closed.error())};
    }
    return std::move(*duplicated);
}

} // namespace

auto LocalIpcAccess::adopt(int descriptor, const DescriptorOperations& descriptors,
                           const IpcOperations& ipc)
    -> std::expected<LocalIpcEndpoint, core::Error> {
    auto owner = DescriptorAccess::adopt(descriptor, descriptors);
    if (!owner) {
        return std::unexpected{std::move(owner.error())};
    }
    return LocalIpcEndpoint{std::move(*owner), ipc};
}

auto LocalIpcAccess::create(const DescriptorOperations& descriptors,
                            const IpcOperations& ipc)
    -> std::expected<LocalIpcChannels, core::Error> {
    constexpr std::string_view operation = "create_local_ipc_channels";
    std::array<int, 2> control{-1, -1};
    std::array<int, 2> events{-1, -1};
    try {
        const auto first = ipc.socket_pair(ipc.context, AF_UNIX,
                                           SOCK_STREAM | SOCK_CLOEXEC, 0, control.data());
        if (first.value != 0 || first.error_number != 0) {
            cleanup_raw(descriptors, control[0]);
            cleanup_raw(descriptors, control[1]);
            return std::unexpected{ipc_error(
                "linux.ipc.create_failed", "Local IPC channel creation failed", operation,
                first.value == -1 && first.error_number != 0 ? "native_failure"
                                                             : "unexpected_native_result",
                first.error_number == 0 ? std::optional<int>{}
                                        : std::optional<int>{first.error_number})};
        }
        const auto second = ipc.socket_pair(ipc.context, AF_UNIX,
                                            SOCK_STREAM | SOCK_CLOEXEC, 0, events.data());
        if (second.value != 0 || second.error_number != 0) {
            cleanup_raw(descriptors, events[0]);
            cleanup_raw(descriptors, events[1]);
            cleanup_raw(descriptors, control[0]);
            cleanup_raw(descriptors, control[1]);
            return std::unexpected{ipc_error(
                "linux.ipc.create_failed", "Local IPC channel creation failed", operation,
                second.value == -1 && second.error_number != 0 ? "native_failure"
                                                               : "unexpected_native_result",
                second.error_number == 0 ? std::optional<int>{}
                                         : std::optional<int>{second.error_number})};
        }

        auto control_controller = DescriptorAccess::adopt(control[0], descriptors);
        auto control_runner = DescriptorAccess::adopt(control[1], descriptors);
        auto event_controller = DescriptorAccess::adopt(events[0], descriptors);
        auto event_runner = DescriptorAccess::adopt(events[1], descriptors);
        if (!control_controller || !control_runner || !event_controller || !event_runner) {
            return std::unexpected{ipc_error(
                "linux.ipc.create_failed", "Local IPC channel returned invalid descriptors",
                operation, "unexpected_native_result")};
        }
        control = {-1, -1};
        events = {-1, -1};

        auto normalized_control_controller = normalize(std::move(*control_controller), descriptors);
        if (!normalized_control_controller) return std::unexpected{std::move(normalized_control_controller.error())};
        auto normalized_control_runner = normalize(std::move(*control_runner), descriptors);
        if (!normalized_control_runner) return std::unexpected{std::move(normalized_control_runner.error())};
        auto normalized_event_controller = normalize(std::move(*event_controller), descriptors);
        if (!normalized_event_controller) return std::unexpected{std::move(normalized_event_controller.error())};
        auto normalized_event_runner = normalize(std::move(*event_runner), descriptors);
        if (!normalized_event_runner) return std::unexpected{std::move(normalized_event_runner.error())};

        return LocalIpcChannels{
            .controller = ControllerIpcEndpoints{
                .control_writer = LocalIpcEndpoint{std::move(*normalized_control_controller), ipc},
                .event_reader = LocalIpcEndpoint{std::move(*normalized_event_controller), ipc},
            },
            .runner = RunnerIpcEndpoints{
                .control_reader = LocalIpcEndpoint{std::move(*normalized_control_runner), ipc},
                .event_writer = LocalIpcEndpoint{std::move(*normalized_event_runner), ipc},
            },
        };
    } catch (const std::exception&) {
        cleanup_raw(descriptors, control[0]); cleanup_raw(descriptors, control[1]);
        cleanup_raw(descriptors, events[0]); cleanup_raw(descriptors, events[1]);
        return std::unexpected{ipc_error(
            "linux.ipc.contained_exception", "Local IPC creation contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        cleanup_raw(descriptors, control[0]); cleanup_raw(descriptors, control[1]);
        cleanup_raw(descriptors, events[0]); cleanup_raw(descriptors, events[1]);
        return std::unexpected{ipc_error(
            "linux.ipc.contained_unknown_failure",
            "Local IPC creation contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

namespace {
auto fixed_io_failure(RawStatusResult result) noexcept -> FixedProcessFailure {
    using Kind = FixedProcessFailureKind;
    if (result.value != -1 || result.error_number <= 0) return {Kind::malformed_result, 0};
    return {result.error_number == EINTR ? Kind::interrupted
                : (result.error_number == EAGAIN || result.error_number == EWOULDBLOCK)
                    ? Kind::would_block : Kind::native_failure, result.error_number};
}
auto fixed_io_progress(RawStatusResult result, std::size_t bound, bool writing) noexcept
    -> std::expected<std::size_t, FixedProcessFailure> {
    if (result.value >= 0 && result.error_number == 0 &&
        static_cast<unsigned long>(result.value) <= bound && (!writing || result.value != 0))
        return static_cast<std::size_t>(result.value);
    return std::unexpected{fixed_io_failure(result)};
}
} // namespace

auto LocalIpcAccess::try_receive(LocalIpcEndpoint& owner, std::span<std::byte> bytes) noexcept
    -> std::expected<std::size_t, FixedProcessFailure> {
    if (!owner.active() || !owner.receive_)
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_state, 0}};
    if (bytes.empty())
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_argument, 0}};
    return fixed_io_progress(owner.receive_(owner.operation_context_, owner.descriptor_.native_handle(),
        bytes.data(), bytes.size(), MSG_DONTWAIT), bytes.size(), false);
}
auto LocalIpcAccess::try_send(LocalIpcEndpoint& owner, std::span<const std::byte> bytes) noexcept
    -> std::expected<std::size_t, FixedProcessFailure> {
    if (!owner.active() || !owner.send_)
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_state, 0}};
    if (bytes.empty())
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_argument, 0}};
    return fixed_io_progress(owner.send_(owner.operation_context_, owner.descriptor_.native_handle(),
        bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL), bytes.size(), true);
}
auto LocalIpcAccess::poll(LocalIpcEndpoint& owner, bool writing, int timeout_ms) noexcept
    -> std::expected<FixedReadiness, FixedProcessFailure> {
    if (!owner.active() || !owner.poll_)
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_state, 0}};
    if (timeout_ms < 0)
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_argument, 0}};
    short events{};
    const short requested = writing ? POLLOUT : POLLIN;
    const auto result = owner.poll_(owner.operation_context_, owner.descriptor_.native_handle(),
                                    requested, timeout_ms, &events);
    if (result.value == 0 && result.error_number == 0 && events == 0)
        return FixedReadiness{.timed_out = true};
    if (result.value == 1 && result.error_number == 0 && (events & POLLNVAL) == 0 &&
        (events & (requested | POLLHUP | POLLERR)) != 0)
        return FixedReadiness{false, (events & requested) != 0,
                              (events & POLLHUP) != 0, (events & POLLERR) != 0};
    return std::unexpected{fixed_io_failure(result)};
}
auto LocalIpcAccess::pause(LocalIpcEndpoint& owner, int timeout_ms) noexcept
    -> std::expected<void, FixedProcessFailure> {
    if (owner.active() || !owner.poll_)
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_state, 0}};
    if (timeout_ms < 0)
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_argument, 0}};
    short events{};
    const auto result = owner.poll_(owner.operation_context_, -1, 0, timeout_ms, &events);
    if (result.value == 0 && result.error_number == 0 && events == 0) return {};
    return std::unexpected{fixed_io_failure(result)};
}

auto LocalIpcAccess::close_fixed(LocalIpcEndpoint& owner) noexcept
    -> std::expected<void, FixedProcessFailure> {
    if (!owner.active())
        return std::unexpected{FixedProcessFailure{FixedProcessFailureKind::invalid_state, 0}};
    const auto result = DescriptorAccess::close_raw(owner.descriptor_);
    if (result.value == 0 && result.error_number == 0) return {};
    return std::unexpected{fixed_io_failure(result)};
}

} // namespace detail

LocalIpcEndpoint::LocalIpcEndpoint(FileDescriptor descriptor,
                                   const detail::IpcOperations& operations) noexcept
    : descriptor_{std::move(descriptor)}, operation_context_{operations.context},
      receive_{operations.receive}, send_{operations.send}, poll_{operations.poll},
      shutdown_{operations.shutdown} {}

auto LocalIpcEndpoint::adopt(int descriptor)
    -> std::expected<LocalIpcEndpoint, core::Error> {
    return detail::LocalIpcAccess::adopt(descriptor,
                                         detail::production_descriptor_operations(),
                                         detail::production_ipc_operations());
}

auto LocalIpcEndpoint::active() const noexcept -> bool { return descriptor_.active(); }

auto LocalIpcEndpoint::receive(std::span<std::byte> buffer)
    -> std::expected<std::size_t, core::Error> {
    constexpr std::string_view operation = "LocalIpcEndpoint.receive";
    if (!active()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_state", "Local IPC endpoint is inactive", operation,
            "endpoint_closed")};
    }
    if (buffer.empty()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_argument", "Local IPC receive buffer is empty", operation,
            "empty_buffer")};
    }
    try {
        const auto result = receive_(operation_context_, descriptor_.native_handle(),
                                     buffer.data(), buffer.size(), 0);
        if (result.value >= 0 && result.error_number == 0 &&
            static_cast<unsigned long>(result.value) <= buffer.size()) {
            return static_cast<std::size_t>(result.value);
        }
        if (result.value != -1 || result.error_number == 0) {
            return std::unexpected{detail::ipc_error(
                "linux.ipc.read_failed", "Local IPC receive returned invalid status", operation,
                "unexpected_native_result")};
        }
        return std::unexpected{detail::ipc_error(
            "linux.ipc.read_failed", "Local IPC receive failed", operation,
            detail::io_classification(result.error_number, false), result.error_number)};
    } catch (const std::exception&) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_exception", "Local IPC receive contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_unknown_failure",
            "Local IPC receive contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto LocalIpcEndpoint::send(std::span<const std::byte> buffer)
    -> std::expected<std::size_t, core::Error> {
    constexpr std::string_view operation = "LocalIpcEndpoint.send";
    if (!active()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_state", "Local IPC endpoint is inactive", operation,
            "endpoint_closed")};
    }
    if (buffer.empty()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_argument", "Local IPC send buffer is empty", operation,
            "empty_buffer")};
    }
    try {
        const auto result = send_(operation_context_, descriptor_.native_handle(), buffer.data(),
                                  buffer.size(), MSG_NOSIGNAL);
        if (result.value >= 0 && result.error_number == 0 &&
            static_cast<unsigned long>(result.value) <= buffer.size()) {
            return static_cast<std::size_t>(result.value);
        }
        if (result.value != -1 || result.error_number == 0) {
            return std::unexpected{detail::ipc_error(
                "linux.ipc.write_failed", "Local IPC send returned invalid status", operation,
                "unexpected_native_result")};
        }
        return std::unexpected{detail::ipc_error(
            "linux.ipc.write_failed", "Local IPC send failed", operation,
            detail::io_classification(result.error_number, true), result.error_number)};
    } catch (const std::exception&) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_exception", "Local IPC send contained an exception", operation,
            "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_unknown_failure", "Local IPC send contained an unknown failure",
            operation, "contained_unknown_failure")};
    }
}

auto LocalIpcEndpoint::readiness(std::int64_t timeout_milliseconds)
    -> std::expected<EndpointReadiness, core::Error> {
    constexpr std::string_view operation = "LocalIpcEndpoint.readiness";
    if (!active()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_state", "Local IPC endpoint is inactive", operation,
            "endpoint_closed")};
    }
    if (timeout_milliseconds < 0 ||
        timeout_milliseconds > std::numeric_limits<int>::max()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_argument", "Local IPC readiness timeout is invalid", operation,
            "invalid_timeout")};
    }
    try {
        short returned_events{};
        const auto result = poll_(operation_context_, descriptor_.native_handle(), POLLIN,
                                  static_cast<int>(timeout_milliseconds), &returned_events);
        if (result.value == 0 && result.error_number == 0) {
            return EndpointReadiness{.timed_out = true};
        }
        if (result.value == 1 && result.error_number == 0) {
            if ((returned_events & POLLNVAL) != 0) {
                return std::unexpected{detail::ipc_error(
                    "linux.ipc.readiness_failed", "Local IPC descriptor is invalid", operation,
                    "invalid_descriptor")};
            }
            EndpointReadiness readiness{
                .timed_out = false,
                .readable = (returned_events & POLLIN) != 0,
                .hangup = (returned_events & POLLHUP) != 0,
                .error = (returned_events & POLLERR) != 0,
            };
            if (!readiness.readable && !readiness.hangup && !readiness.error) {
                return std::unexpected{detail::ipc_error(
                    "linux.ipc.readiness_failed", "Local IPC poll returned no known event",
                    operation, "unexpected_native_result")};
            }
            return readiness;
        }
        if (result.value == -1 && result.error_number != 0) {
            return std::unexpected{detail::ipc_error(
                "linux.ipc.readiness_failed", "Local IPC readiness failed", operation,
                result.error_number == EINTR ? "interrupted" : "native_failure",
                result.error_number)};
        }
        return std::unexpected{detail::ipc_error(
            "linux.ipc.readiness_failed", "Local IPC poll returned invalid status", operation,
            "unexpected_native_result")};
    } catch (const std::exception&) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_exception", "Local IPC readiness contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_unknown_failure",
            "Local IPC readiness contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto LocalIpcEndpoint::observe_writable(std::int64_t timeout_milliseconds)
    -> std::expected<WritableReadiness, core::Error> {
    constexpr std::string_view operation = "LocalIpcEndpoint.observe_writable";
    if (!active()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_state", "Local IPC endpoint is inactive", operation,
            "endpoint_closed")};
    }
    if (timeout_milliseconds < 0 ||
        timeout_milliseconds > std::numeric_limits<int>::max()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_argument", "Local IPC readiness timeout is invalid", operation,
            "invalid_timeout")};
    }
    try {
        short returned_events{};
        const auto result = poll_(operation_context_, descriptor_.native_handle(), POLLOUT,
                                  static_cast<int>(timeout_milliseconds), &returned_events);
        if (result.value == 0 && result.error_number == 0) {
            return WritableReadiness{.timed_out = true};
        }
        if (result.value == 1 && result.error_number == 0) {
            if ((returned_events & POLLNVAL) != 0) {
                return std::unexpected{detail::ipc_error(
                    "linux.ipc.readiness_failed", "Local IPC descriptor is invalid", operation,
                    "invalid_descriptor")};
            }
            WritableReadiness readiness{
                .timed_out = false,
                .writable = (returned_events & POLLOUT) != 0,
                .hangup = (returned_events & POLLHUP) != 0,
                .error = (returned_events & POLLERR) != 0,
            };
            if (!readiness.writable && !readiness.hangup && !readiness.error) {
                return std::unexpected{detail::ipc_error(
                    "linux.ipc.readiness_failed", "Local IPC poll returned no known event",
                    operation, "unexpected_native_result")};
            }
            return readiness;
        }
        if (result.value == -1 && result.error_number != 0) {
            return std::unexpected{detail::ipc_error(
                "linux.ipc.readiness_failed", "Local IPC readiness failed", operation,
                result.error_number == EINTR ? "interrupted" : "native_failure",
                result.error_number)};
        }
        return std::unexpected{detail::ipc_error(
            "linux.ipc.readiness_failed", "Local IPC poll returned invalid status", operation,
            "unexpected_native_result")};
    } catch (const std::exception&) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_exception", "Local IPC readiness contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_unknown_failure",
            "Local IPC readiness contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto LocalIpcEndpoint::close() -> std::expected<void, core::Error> {
    return descriptor_.close();
}

auto LocalIpcEndpoint::shutdown_read() -> std::expected<void, core::Error> {
    constexpr std::string_view operation = "LocalIpcEndpoint.shutdown_read";
    if (!active()) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.invalid_state", "Local IPC endpoint is inactive", operation,
            "endpoint_closed")};
    }
    if (read_shutdown_) {
        return {};
    }
    try {
        const auto result = shutdown_(operation_context_, descriptor_.native_handle(), SHUT_RD);
        if (result.value == 0 && result.error_number == 0) {
            read_shutdown_ = true;
            return {};
        }
        if (result.value != -1 || result.error_number == 0) {
            return std::unexpected{detail::ipc_error(
                "linux.ipc.shutdown_failed", "Local IPC read shutdown returned invalid status",
                operation, "unexpected_native_result")};
        }
        return std::unexpected{detail::ipc_error(
            "linux.ipc.shutdown_failed", "Local IPC read shutdown failed", operation,
            result.error_number == EINTR ? "interrupted" : "native_failure",
            result.error_number)};
    } catch (const std::exception&) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_exception", "Local IPC read shutdown contained an exception",
            operation, "contained_exception")};
    } catch (...) {
        return std::unexpected{detail::ipc_error(
            "linux.ipc.contained_unknown_failure",
            "Local IPC read shutdown contained an unknown failure", operation,
            "contained_unknown_failure")};
    }
}

auto create_local_ipc_channels() -> std::expected<LocalIpcChannels, core::Error> {
    return detail::LocalIpcAccess::create(detail::production_descriptor_operations(),
                                          detail::production_ipc_operations());
}

} // namespace epycbench::platform_linux
