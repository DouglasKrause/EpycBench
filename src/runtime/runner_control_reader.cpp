#include "epycbench/runtime/runner_control_reader.hpp"

#include "runtime/platform_ipc_adapters.hpp"
#include "runtime/runner_control_reader_access.hpp"

#include "epycbench/platform_linux/local_ipc.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace epycbench::runtime {
namespace {

[[nodiscard]] auto reader_error(std::string code, std::string_view classification,
                                std::string_view phase, const core::Error* underlying = nullptr)
    -> core::Error {
    nlohmann::json context{
        {"channel", "control"}, {"reader_phase", phase}, {"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        if (underlying->os_error) {
            os_error = core::OsError{
                .value = underlying->os_error->value, .domain = "errno", .message = ""};
        }
    }
    return core::Error{
        .category = core::ErrorCategory::runtime,
        .code = core::StableErrorCode{std::move(code)},
        .message = "Runner control reader operation failed",
        .operation = "runner_control_reader",
        .os_error = std::move(os_error),
        .context = std::move(context),
        .severity = core::ErrorSeverity::error,
        .continuation = core::ContinuationPolicy::abort_command,
    };
}

[[nodiscard]] auto contained_error(bool unknown, std::string_view phase) -> core::Error {
    return reader_error(unknown ? "runtime.runner_control_reader.contained_unknown_failure"
                                : "runtime.runner_control_reader.contained_exception",
                        unknown ? "contained_unknown_failure" : "contained_exception", phase);
}

[[nodiscard]] auto endpoint_stop(void* context) -> std::expected<void, core::Error> {
    return static_cast<platform_linux::LocalIpcEndpoint*>(context)->shutdown_read();
}

} // namespace

struct RunnerControlReadCapability::State {
    enum class Phase {
        open_idle,
        open_reading,
        local_stop_requested,
        local_stop_terminal,
        peer_eof_terminal,
        peer_error_terminal,
    };

    std::mutex mutex;
    Phase phase{Phase::open_idle};
    ipc::ByteReader* reader{};
    std::unique_ptr<ipc::ByteReader> owned_reader;
    detail::RunnerControlStopOperation stop;
    bool stop_in_progress{};
    bool stop_succeeded{};
    std::atomic<bool> stop_requested_evidence{};
    std::atomic<bool> terminal_evidence{};
};

namespace detail {

struct RunnerControlReaderFactory {
    [[nodiscard]] static auto create(ipc::ByteReader* reader,
                                     std::unique_ptr<ipc::ByteReader> owned_reader,
                                     RunnerControlStopOperation stop)
        -> std::expected<RunnerControlReadCapability, core::Error>;
};

auto RunnerControlReaderFactory::create(ipc::ByteReader* reader,
                                        std::unique_ptr<ipc::ByteReader> owned_reader,
                                        RunnerControlStopOperation stop)
    -> std::expected<RunnerControlReadCapability, core::Error> {
    try {
        if (reader == nullptr) {
            return std::unexpected{reader_error("runtime.runner_control_reader.invalid_argument",
                                                "missing_reader", "construction")};
        }
        if (stop.request_stop == nullptr) {
            return std::unexpected{reader_error("runtime.runner_control_reader.invalid_argument",
                                                "missing_stop_operation", "construction")};
        }
        auto state = std::make_unique<RunnerControlReadCapability::State>();
        state->reader = reader;
        state->owned_reader = std::move(owned_reader);
        state->stop = stop;
        return RunnerControlReadCapability{std::move(state)};
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "construction")};
    } catch (...) {
        return std::unexpected{contained_error(true, "construction")};
    }
}

} // namespace detail

RunnerControlReadCapability::RunnerControlReadCapability(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

RunnerControlReadCapability::RunnerControlReadCapability(
    RunnerControlReadCapability&& other) noexcept
    : state_{std::move(other.state_)} {}

RunnerControlReadCapability::~RunnerControlReadCapability() noexcept = default;

auto RunnerControlReadCapability::read_next()
    -> std::expected<RunnerControlReadResult, core::Error> {
    try {
        if (!state_) {
            return std::unexpected{
                reader_error("runtime.runner_control_reader.invalid_state", "moved_from", "read")};
        }
        {
            std::lock_guard lock{state_->mutex};
            switch (state_->phase) {
            case State::Phase::open_idle:
                state_->phase = State::Phase::open_reading;
                break;
            case State::Phase::open_reading:
                return std::unexpected{reader_error("runtime.runner_control_reader.invalid_state",
                                                    "concurrent_read", "read")};
            case State::Phase::local_stop_requested:
                state_->phase = State::Phase::local_stop_terminal;
                state_->terminal_evidence.store(true, std::memory_order_release);
                return RunnerControlReadResult{RunnerControlLocalStop{}};
            case State::Phase::local_stop_terminal:
            case State::Phase::peer_eof_terminal:
            case State::Phase::peer_error_terminal:
                return std::unexpected{reader_error("runtime.runner_control_reader.invalid_state",
                                                    "read_after_terminal", "read")};
            }
        }

        auto frame = ipc::read_frame(*state_->reader);

        std::lock_guard lock{state_->mutex};
        if (state_->phase == State::Phase::local_stop_requested) {
            state_->phase = State::Phase::local_stop_terminal;
            state_->terminal_evidence.store(true, std::memory_order_release);
            return RunnerControlReadResult{RunnerControlLocalStop{}};
        }
        if (state_->phase != State::Phase::open_reading) {
            state_->phase = State::Phase::peer_error_terminal;
            state_->terminal_evidence.store(true, std::memory_order_release);
            return std::unexpected{reader_error("runtime.runner_control_reader.invariant_failed",
                                                "invalid_read_state", "publication")};
        }
        if (!frame) {
            state_->phase = State::Phase::peer_error_terminal;
            state_->terminal_evidence.store(true, std::memory_order_release);
            return std::unexpected{std::move(frame.error())};
        }
        if (std::holds_alternative<ipc::EndOfStream>(*frame)) {
            state_->phase = State::Phase::peer_eof_terminal;
            state_->terminal_evidence.store(true, std::memory_order_release);
            return RunnerControlReadResult{RunnerControlPeerEof{}};
        }
        state_->phase = State::Phase::open_idle;
        return RunnerControlReadResult{std::move(std::get<ipc::Envelope>(*frame))};
    } catch (const std::exception&) {
        if (state_) {
            std::lock_guard lock{state_->mutex};
            if (state_->phase == State::Phase::local_stop_requested) {
                state_->phase = State::Phase::local_stop_terminal;
            } else {
                state_->phase = State::Phase::peer_error_terminal;
            }
            state_->terminal_evidence.store(true, std::memory_order_release);
        }
        return std::unexpected{contained_error(false, "read")};
    } catch (...) {
        if (state_) {
            std::lock_guard lock{state_->mutex};
            if (state_->phase == State::Phase::local_stop_requested) {
                state_->phase = State::Phase::local_stop_terminal;
            } else {
                state_->phase = State::Phase::peer_error_terminal;
            }
            state_->terminal_evidence.store(true, std::memory_order_release);
        }
        return std::unexpected{contained_error(true, "read")};
    }
}

auto RunnerControlReadCapability::request_stop() -> std::expected<void, core::Error> {
    try {
        if (!state_) {
            return std::unexpected{
                reader_error("runtime.runner_control_reader.invalid_state", "moved_from", "stop")};
        }
        {
            std::lock_guard lock{state_->mutex};
            if (state_->phase == State::Phase::peer_eof_terminal ||
                state_->phase == State::Phase::peer_error_terminal) {
                return {};
            }
            if (state_->stop_succeeded) {
                return {};
            }
            if (state_->stop_in_progress) {
                return std::unexpected{reader_error("runtime.runner_control_reader.invalid_state",
                                                    "stop_in_progress", "stop")};
            }
            if (state_->phase == State::Phase::open_idle ||
                state_->phase == State::Phase::open_reading) {
                state_->phase = State::Phase::local_stop_requested;
                state_->stop_requested_evidence.store(true, std::memory_order_release);
            }
            state_->stop_in_progress = true;
        }

        auto stopped = state_->stop.request_stop(state_->stop.context);

        std::lock_guard lock{state_->mutex};
        state_->stop_in_progress = false;
        if (!stopped) {
            return std::unexpected{reader_error("runtime.runner_control_reader.stop_failed",
                                                "stop_operation_failed", "stop", &stopped.error())};
        }
        state_->stop_succeeded = true;
        return {};
    } catch (const std::exception&) {
        if (state_) {
            std::lock_guard lock{state_->mutex};
            state_->stop_in_progress = false;
        }
        return std::unexpected{contained_error(false, "stop")};
    } catch (...) {
        if (state_) {
            std::lock_guard lock{state_->mutex};
            state_->stop_in_progress = false;
        }
        return std::unexpected{contained_error(true, "stop")};
    }
}

auto RunnerControlReadCapability::stop_requested() const noexcept -> bool {
    return state_ && state_->stop_requested_evidence.load(std::memory_order_acquire);
}

auto RunnerControlReadCapability::terminal() const noexcept -> bool {
    return state_ && state_->terminal_evidence.load(std::memory_order_acquire);
}

auto detail::RunnerControlReaderAccess::create(ipc::ByteReader* reader,
                                               detail::RunnerControlStopOperation stop)
    -> std::expected<RunnerControlReadCapability, core::Error> {
    return detail::RunnerControlReaderFactory::create(reader, nullptr, stop);
}

auto make_runner_control_read_capability(platform_linux::LocalIpcEndpoint& endpoint)
    -> std::expected<RunnerControlReadCapability, core::Error> {
    try {
        auto reader = std::make_unique<detail::EndpointByteReader>(endpoint);
        auto* borrowed = reader.get();
        return detail::RunnerControlReaderFactory::create(
            borrowed, std::move(reader),
            detail::RunnerControlStopOperation{&endpoint, endpoint_stop});
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "construction")};
    } catch (...) {
        return std::unexpected{contained_error(true, "construction")};
    }
}

} // namespace epycbench::runtime
