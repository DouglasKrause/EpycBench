#include "runtime/controller_ipc_adapters.hpp"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>

namespace epycbench::runtime::detail {
namespace {

[[nodiscard]] auto classification_of(const core::Error& error) -> std::string_view {
    if (!error.context.is_object()) return {};
    const auto item = error.context.find("classification");
    if (item == error.context.end() || !item->is_string()) return {};
    return item->get_ref<const std::string&>();
}

[[nodiscard]] auto controller_error(std::string_view code, std::string_view operation,
                                    std::string_view classification,
                                    std::string_view channel = {},
                                    std::optional<core::OsError> os_error = std::nullopt,
                                    std::string_view underlying = {}) -> core::Error {
    nlohmann::json context{{"classification", classification}};
    if (!channel.empty()) context["channel"] = channel;
    if (!underlying.empty()) context["underlying_code"] = underlying;
    return core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{std::string{code}},
        .message = "Controller IPC operation failed",
        .operation = std::string{operation},
        .os_error = std::move(os_error),
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto project_io(const core::Error& error, std::string_view operation,
                              std::string_view channel) -> core::Error {
    auto classification = classification_of(error);
    if (error.code.value() == "linux.ipc.invalid_state") classification = "endpoint_closed";
    if (classification.empty() || classification == "invalid_timeout") {
        classification = "native_failure";
    }
    return controller_error("runtime.ipc.io_failure", operation, classification, channel,
                            error.os_error, error.code.value());
}

[[nodiscard]] auto timeout_error(std::string_view operation, std::string_view classification,
                                 std::string_view channel) -> core::Error {
    return controller_error("runtime.controller.timeout", operation, classification, channel);
}

[[nodiscard]] auto uncertain_io(const core::Error& error, std::string_view operation,
                                std::string_view channel) -> core::Error {
    return controller_error("runtime.ipc.io_failure", operation, "partial_io", channel,
                            error.os_error, error.code.value());
}

[[nodiscard]] auto contained(std::string_view operation, std::string_view channel,
                             bool unknown) -> core::Error {
    return controller_error(unknown ? "runtime.controller.contained_unknown_failure"
                                    : "runtime.controller.contained_exception",
                            operation,
                            unknown ? "contained_unknown_failure" : "contained_exception",
                            channel);
}

} // namespace

auto production_controller_clock() noexcept -> ControllerClock {
    static const auto now = [](void*) noexcept { return std::chrono::steady_clock::now(); };
    return ControllerClock{nullptr, now};
}

auto checked_deadline(ControllerClock clock, std::chrono::milliseconds duration)
    -> std::expected<ControllerTimePoint, core::Error> {
    constexpr std::string_view operation = "controller.checked_deadline";
    try {
        if (clock.now == nullptr || duration.count() < 0) {
            return std::unexpected{controller_error(
                "runtime.controller.invalid_argument", operation,
                "invalid_lifecycle_config")};
        }
        const auto current = clock.now(clock.context);
        const auto maximum_milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                ControllerTimePoint::duration::max());
        if (duration > maximum_milliseconds) {
            return std::unexpected{controller_error(
                "runtime.controller.invalid_argument", operation,
                "invalid_lifecycle_config")};
        }
        const auto delta = std::chrono::duration_cast<ControllerTimePoint::duration>(duration);
        if (delta.count() < 0 || current > ControllerTimePoint::max() - delta) {
            return std::unexpected{controller_error(
                "runtime.controller.invalid_argument", operation,
                "invalid_lifecycle_config")};
        }
        return current + delta;
    } catch (const std::exception&) {
        return std::unexpected{contained(operation, {}, false)};
    } catch (...) {
        return std::unexpected{contained(operation, {}, true)};
    }
}

auto poll_timeout(ControllerClock clock, ControllerTimePoint deadline)
    -> std::expected<int, core::Error> {
    constexpr std::string_view operation = "controller.poll_timeout";
    try {
        if (clock.now == nullptr) {
            return std::unexpected{controller_error(
                "runtime.controller.invalid_argument", operation,
                "invalid_lifecycle_config")};
        }
        const auto current = clock.now(clock.context);
        if (current >= deadline) return 0;
        const auto remaining = deadline - current;
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (milliseconds < remaining) milliseconds += std::chrono::milliseconds{1};
        if (milliseconds.count() > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(milliseconds.count());
    } catch (const std::exception&) {
        return std::unexpected{contained(operation, {}, false)};
    } catch (...) {
        return std::unexpected{contained(operation, {}, true)};
    }
}

DeadlineControlWriter::DeadlineControlWriter(
    platform_linux::LocalIpcEndpoint& endpoint, ControllerClock clock,
    const ControllerTimePoint& deadline, std::string_view timeout_classification,
    ControllerInterruptionPolicy interruption_policy) noexcept
    : endpoint_{&endpoint}, clock_{clock}, deadline_{&deadline},
      timeout_classification_{timeout_classification}, interruption_policy_{interruption_policy} {}

auto DeadlineControlWriter::write(std::span<const std::byte> bytes)
    -> std::expected<std::size_t, core::Error> {
    constexpr std::string_view operation = "DeadlineControlWriter.write";
    try {
        auto timeout = poll_timeout(clock_, *deadline_);
        if (!timeout) return std::unexpected{std::move(timeout.error())};
        if (*timeout == 0) {
            return std::unexpected{timeout_error(operation, timeout_classification_, "control")};
        }
        auto readiness = endpoint_->observe_writable(*timeout);
        if (!readiness) {
            if (interruption_policy_ == ControllerInterruptionPolicy::stop_after_frame_progress &&
                progress_ != 0U && classification_of(readiness.error()) == "interrupted")
                return std::unexpected{uncertain_io(readiness.error(), operation, "control")};
            return std::unexpected{project_io(readiness.error(), operation, "control")};
        }
        if (readiness->timed_out) {
            return std::unexpected{timeout_error(operation, timeout_classification_, "control")};
        }
        if (!readiness->writable) {
            return std::unexpected{controller_error(
                "runtime.ipc.io_failure", operation, "native_failure", "control",
                std::nullopt, "linux.ipc.readiness_failed")};
        }
        auto sent = endpoint_->send(bytes);
        if (!sent) {
            if (interruption_policy_ == ControllerInterruptionPolicy::stop_after_frame_progress &&
                progress_ != 0U && classification_of(sent.error()) == "interrupted")
                return std::unexpected{uncertain_io(sent.error(), operation, "control")};
            return std::unexpected{project_io(sent.error(), operation, "control")};
        }
        progress_ += *sent;
        return *sent;
    } catch (const std::exception&) {
        return std::unexpected{contained(operation, "control", false)};
    } catch (...) {
        return std::unexpected{contained(operation, "control", true)};
    }
}

auto DeadlineControlWriter::progress() const noexcept -> std::size_t { return progress_; }
void DeadlineControlWriter::reset_progress() noexcept { progress_ = 0; }

DeadlineEventReader::DeadlineEventReader(
    platform_linux::LocalIpcEndpoint& endpoint, ControllerClock clock,
    const ControllerTimePoint& deadline, std::string_view timeout_classification,
    ControllerInterruptionPolicy interruption_policy) noexcept
    : endpoint_{&endpoint}, clock_{clock}, deadline_{&deadline},
      timeout_classification_{timeout_classification}, interruption_policy_{interruption_policy} {}

auto DeadlineEventReader::read(std::span<std::byte> bytes)
    -> std::expected<std::size_t, core::Error> {
    constexpr std::string_view operation = "DeadlineEventReader.read";
    try {
        auto timeout = poll_timeout(clock_, *deadline_);
        if (!timeout) return std::unexpected{std::move(timeout.error())};
        if (*timeout == 0) {
            return std::unexpected{timeout_error(operation, timeout_classification_, "event")};
        }
        auto readiness = endpoint_->readiness(*timeout);
        if (!readiness) {
            if (interruption_policy_ == ControllerInterruptionPolicy::stop_after_frame_progress &&
                progress_ != 0U && classification_of(readiness.error()) == "interrupted")
                return std::unexpected{uncertain_io(readiness.error(), operation, "event")};
            return std::unexpected{project_io(readiness.error(), operation, "event")};
        }
        if (readiness->timed_out) {
            return std::unexpected{timeout_error(operation, timeout_classification_, "event")};
        }
        if (!readiness->readable && !readiness->hangup) {
            return std::unexpected{controller_error(
                "runtime.ipc.io_failure", operation, "native_failure", "event",
                std::nullopt, "linux.ipc.readiness_failed")};
        }
        auto received = endpoint_->receive(bytes);
        if (!received) {
            if (interruption_policy_ == ControllerInterruptionPolicy::stop_after_frame_progress &&
                progress_ != 0U && classification_of(received.error()) == "interrupted")
                return std::unexpected{uncertain_io(received.error(), operation, "event")};
            return std::unexpected{project_io(received.error(), operation, "event")};
        }
        progress_ += *received;
        return *received;
    } catch (const std::exception&) {
        return std::unexpected{contained(operation, "event", false)};
    } catch (...) {
        return std::unexpected{contained(operation, "event", true)};
    }
}

auto DeadlineEventReader::progress() const noexcept -> std::size_t { return progress_; }
void DeadlineEventReader::reset_progress() noexcept { progress_ = 0; }

} // namespace epycbench::runtime::detail
