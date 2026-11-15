#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/runtime/ipc/payloads.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace epycbench::runtime {

struct ServiceThreadReadyEvidence;

namespace detail {
struct RunnerControlMonitorAccess;
}

class RunnerCancellation {
  public:
    RunnerCancellation() noexcept = default;
    RunnerCancellation(const RunnerCancellation&) = delete;
    auto operator=(const RunnerCancellation&) -> RunnerCancellation& = delete;
    RunnerCancellation(RunnerCancellation&&) = delete;
    auto operator=(RunnerCancellation&&) -> RunnerCancellation& = delete;

    [[nodiscard]] auto try_request(ipc::CancelReason reason) noexcept -> bool;
    [[nodiscard]] auto requested() const noexcept -> bool;
    [[nodiscard]] auto reason() const noexcept -> std::optional<ipc::CancelReason>;

  private:
    enum class Code : std::uint8_t {
        none = 0,
        user_interrupt = 1,
        controller_shutdown = 2,
        controller_timeout = 3
    };

    [[nodiscard]] static auto encode(ipc::CancelReason reason) noexcept -> std::optional<Code>;
    [[nodiscard]] static auto decode(Code code) noexcept -> std::optional<ipc::CancelReason>;

    std::atomic<Code> state_{Code::none};

    friend struct detail::RunnerControlMonitorAccess;
};

class RunnerControlMonitorResult {
  public:
    [[nodiscard]] static auto create(std::optional<ipc::CancelReason> cancellation_reason,
                                     bool control_eof_observed) noexcept
        -> std::expected<RunnerControlMonitorResult, core::Error>;

    [[nodiscard]] auto cancellation_reason() const noexcept -> std::optional<ipc::CancelReason> {
        return cancellation_reason_;
    }
    [[nodiscard]] auto control_eof_observed() const noexcept -> bool {
        return control_eof_observed_;
    }
    auto operator==(const RunnerControlMonitorResult&) const -> bool = default;

  private:
    RunnerControlMonitorResult(std::optional<ipc::CancelReason> cancellation_reason,
                               bool control_eof_observed) noexcept
        : cancellation_reason_{cancellation_reason}, control_eof_observed_{control_eof_observed} {}

    std::optional<ipc::CancelReason> cancellation_reason_;
    bool control_eof_observed_{};
};

class RunnerControlMonitor {
  public:
    // A concrete callback owns control-frame monitoring after placement sequence zero. It must
    // use RunnerControlState, validate the optional cancel request, publish at most one reason,
    // and retain borrowed arguments only until stop_and_join returns. It must not emit events,
    // invoke a backend, or select a terminal outcome. stop_and_join must explicitly unblock any
    // pending read and join without relying on peer EOF. Every normal return (value or error), and
    // every exception that it permits to propagate, certifies that cleanup is complete.
    // An unsuccessful or throwing start must likewise leave no active resource and retain no
    // borrowed reference.
    using StartFunction = std::expected<void, core::Error> (*)(void*);
    using StopAndJoinFunction = std::expected<RunnerControlMonitorResult, core::Error> (*)(void*);

    [[nodiscard]] static auto create(void* context, StartFunction start,
                                     StopAndJoinFunction stop_and_join) noexcept
        -> std::expected<RunnerControlMonitor, core::Error>;
    [[nodiscard]] static auto create_owned(std::shared_ptr<void> context, StartFunction start,
                                           StopAndJoinFunction stop_and_join) noexcept
        -> std::expected<RunnerControlMonitor, core::Error>;

    RunnerControlMonitor(RunnerControlMonitor&& other) noexcept;
    RunnerControlMonitor(const RunnerControlMonitor&) = delete;
    auto operator=(const RunnerControlMonitor&) -> RunnerControlMonitor& = delete;
    auto operator=(RunnerControlMonitor&&) -> RunnerControlMonitor& = delete;
    ~RunnerControlMonitor() noexcept;

    [[nodiscard]] auto start() -> std::expected<void, core::Error>;
    [[nodiscard]] auto stop_and_join() -> std::expected<RunnerControlMonitorResult, core::Error>;

    [[nodiscard]] auto active() const noexcept -> bool;
    [[nodiscard]] auto completed() const noexcept -> bool;

  private:
    enum class State { created, start_failed, active, completed, moved_from };

    RunnerControlMonitor(void* context, StartFunction start_callback,
                         StopAndJoinFunction stop_callback) noexcept
        : context_{context}, start_{start_callback}, stop_and_join_{stop_callback} {}

    [[nodiscard]] auto destroying_active_is_fatal() const noexcept -> bool;

    void* context_{};
    StartFunction start_{};
    StopAndJoinFunction stop_and_join_{};
    std::shared_ptr<void> owned_context_;
    State state_{State::created};

    friend struct detail::RunnerControlMonitorAccess;
    friend auto service_thread_readiness(const RunnerControlMonitor&) noexcept
        -> std::expected<std::reference_wrapper<const ServiceThreadReadyEvidence>, core::Error>;
};

} // namespace epycbench::runtime
