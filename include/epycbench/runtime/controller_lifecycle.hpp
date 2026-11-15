#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/platform_linux/child_process.hpp"
#include "epycbench/runtime/ipc/payloads.hpp"

#include <chrono>
#include <expected>
#include <optional>
#include <string>

namespace epycbench::runtime {

struct ControllerLifecycleConfig {
    std::chrono::milliseconds startup_timeout;
    std::chrono::milliseconds execution_timeout;
    std::chrono::milliseconds cancellation_grace;
    std::chrono::milliseconds completion_grace;
    std::chrono::milliseconds termination_grace;
    std::chrono::milliseconds observation_quantum;
};

struct ControllerRequest {
    std::string executable;
    ipc::RequestId request_id;
    ipc::PlacementRequest placement;
    ControllerLifecycleConfig lifecycle;
};

enum class ControllerCancellation { none, user_interrupt, controller_shutdown };

struct CancellationObserver {
    void* context{};
    ControllerCancellation (*observe)(void* context) noexcept{nullptr};
};

struct ControllerOutcome {
    ipc::RequestId request_id;
    ipc::Envelope terminal_event;
    platform_linux::ProcessExitStatus process_status;
    std::optional<ipc::CancelReason> cancellation_reason;
};

struct ControllerFailure {
    core::Error primary;
    bool request_accepted{};
    std::optional<ipc::Envelope> terminal_event;
    bool event_eof_observed{};
    std::optional<platform_linux::ProcessExitStatus> process_status;
    std::optional<ipc::CancelReason> cancellation_reason;
};

using ControllerResult = std::expected<ControllerOutcome, ControllerFailure>;

[[nodiscard]] auto run_controller_request(
    const ControllerRequest& request,
    CancellationObserver cancellation = {}) -> ControllerResult;

} // namespace epycbench::runtime
