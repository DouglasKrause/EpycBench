#include "epycbench/runtime/production_control_monitor.hpp"

#include "runtime/production_control_monitor_access.hpp"

#include "epycbench/platform_linux/thread_affinity.hpp"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace epycbench::runtime {
namespace {

[[nodiscard]] auto service_error(std::string code, std::string_view classification,
                                 std::string_view phase, const core::Error* underlying = nullptr)
    -> core::Error {
    nlohmann::json context{{"service_role", "control_monitor"},
                           {"service_phase", phase},
                           {"classification", classification}};
    std::optional<core::OsError> os_error;
    if (underlying != nullptr) {
        context["underlying_code"] = underlying->code.value();
        if (underlying->os_error)
            os_error = core::OsError{
                .value = underlying->os_error->value, .domain = "errno", .message = ""};
    }
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Placed service control monitor failed",
            .operation = "service_control_monitor",
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}

[[nodiscard]] auto production_tid(void*)
    -> std::expected<platform_linux::LinuxThreadId, core::Error> {
    return platform_linux::current_thread_id();
}
[[nodiscard]] auto production_set(void*, const core::CpuSet& cpus)
    -> std::expected<void, core::Error> {
    return platform_linux::set_current_thread_affinity(cpus);
}
[[nodiscard]] auto production_get(void*) -> std::expected<core::CpuSet, core::Error> {
    return platform_linux::get_current_thread_affinity();
}
[[nodiscard]] auto production_cpu(void*) -> std::expected<core::LinuxCpuId, core::Error> {
    return platform_linux::current_cpu();
}

struct MonitorState : std::enable_shared_from_this<MonitorState> {
    MonitorState(ServicePlacementPlan plan, std::unique_ptr<ipc::RunnerControlState> state,
                 RunnerControlReadCapability& reader, RunnerCancellation& cancel,
                 detail::ServicePlatformOperations operations)
        : placement{std::move(plan)}, control_state{std::move(state)}, control{&reader},
          cancellation{&cancel}, platform{operations} {}

    ServicePlacementPlan placement;
    std::unique_ptr<ipc::RunnerControlState> control_state;
    RunnerControlReadCapability* control{};
    RunnerCancellation* cancellation{};
    detail::ServicePlatformOperations platform;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<ServiceThreadReadyEvidence> readiness;
    std::optional<core::Error> failure;
    std::optional<ipc::CancelReason> recorded_reason;
    bool startup_complete{};
    bool eof{};
    bool completed{};
    std::jthread thread;
};

void monitor_entry(const std::shared_ptr<MonitorState>& state) noexcept {
    try {
        auto ready = detail::ProductionControlMonitorAccess::startup(
            state->placement.assignments().front(), state->platform);
        {
            std::lock_guard lock{state->mutex};
            if (ready)
                state->readiness = std::move(*ready);
            else
                state->failure = std::move(ready.error());
            state->startup_complete = true;
        }
        state->condition.notify_all();
        if (!ready)
            return;

        while (true) {
            auto step = detail::ProductionControlMonitorAccess::monitor_once(
                *state->control_state, *state->control, *state->cancellation,
                state->recorded_reason, state->eof);
            if (!step) {
                std::lock_guard lock{state->mutex};
                state->failure = std::move(step.error());
                break;
            }
            if (!*step)
                break;
        }
    } catch (const std::exception&) {
        std::lock_guard lock{state->mutex};
        state->failure = service_error("runtime.service_threads.contained_exception",
                                       "contained_exception", "monitor");
        state->startup_complete = true;
        state->condition.notify_all();
    } catch (...) {
        std::lock_guard lock{state->mutex};
        state->failure = service_error("runtime.service_threads.contained_unknown_failure",
                                       "contained_unknown_failure", "monitor");
        state->startup_complete = true;
        state->condition.notify_all();
    }
    {
        std::lock_guard lock{state->mutex};
        state->completed = true;
    }
    state->condition.notify_all();
}

auto monitor_start(void* opaque) -> std::expected<void, core::Error> {
    auto& state = *static_cast<MonitorState*>(opaque);
    try {
        auto owner = state.shared_from_this();
        state.thread = std::jthread([owner] { monitor_entry(owner); });
        std::unique_lock lock{state.mutex};
        state.condition.wait(lock, [&] { return state.startup_complete; });
        if (state.failure) {
            lock.unlock();
            if (state.thread.joinable())
                state.thread.join();
            state.control = nullptr;
            state.cancellation = nullptr;
            return std::unexpected{*state.failure};
        }
        return {};
    } catch (const std::exception&) {
        if (state.thread.joinable())
            state.thread.join();
        state.control = nullptr;
        state.cancellation = nullptr;
        return std::unexpected{service_error("runtime.service_threads.start_failed",
                                             "thread_launch_failed", "startup")};
    } catch (...) {
        if (state.thread.joinable())
            state.thread.join();
        state.control = nullptr;
        state.cancellation = nullptr;
        return std::unexpected{service_error("runtime.service_threads.contained_unknown_failure",
                                             "contained_unknown_failure", "startup")};
    }
}

auto monitor_stop(void* opaque) -> std::expected<RunnerControlMonitorResult, core::Error> {
    auto& state = *static_cast<MonitorState*>(opaque);
    std::optional<core::Error> stop_failure;
    try {
        auto stopped = state.control->request_stop();
        if (!stopped)
            stop_failure = service_error("runtime.service_threads.stop_failed",
                                         "monitor_unblock_failed", "stop", &stopped.error());
        if (state.thread.joinable())
            state.thread.join();
        const auto published_reason = state.cancellation->reason();
        state.control = nullptr;
        state.cancellation = nullptr;
        if (state.failure)
            return std::unexpected{*state.failure};
        if (stop_failure)
            return std::unexpected{*stop_failure};
        if (state.recorded_reason != published_reason)
            return std::unexpected{service_error("runtime.service_threads.invariant_failed",
                                                 "cancel_publication_mismatch", "stop")};
        return RunnerControlMonitorResult::create(state.recorded_reason, state.eof);
    } catch (const std::exception&) {
        if (state.thread.joinable())
            state.thread.join();
        state.control = nullptr;
        state.cancellation = nullptr;
        return std::unexpected{service_error("runtime.service_threads.contained_exception",
                                             "contained_exception", "stop")};
    } catch (...) {
        if (state.thread.joinable())
            state.thread.join();
        state.control = nullptr;
        state.cancellation = nullptr;
        return std::unexpected{service_error("runtime.service_threads.contained_unknown_failure",
                                             "contained_unknown_failure", "stop")};
    }
}

auto create_monitor(void* opaque, const AdmittedPlacementRequest& admitted,
                    std::unique_ptr<ipc::RunnerControlState> control_state,
                    RunnerControlReadCapability& control, RunnerCancellation& cancellation)
    -> std::expected<RunnerControlMonitor, core::Error>;

struct FactoryState {
    detail::ServicePlatformOperations platform;
};

auto create_monitor(void* opaque, const AdmittedPlacementRequest& admitted,
                    std::unique_ptr<ipc::RunnerControlState> control_state,
                    RunnerControlReadCapability& control, RunnerCancellation& cancellation)
    -> std::expected<RunnerControlMonitor, core::Error> {
    try {
        auto placement = ServicePlacementPlan::create(admitted);
        if (!placement)
            return std::unexpected{std::move(placement.error())};
        if (!control_state)
            return std::unexpected{service_error("runtime.service_threads.invalid_argument",
                                                 "missing_control_state", "factory")};
        auto state = std::make_shared<MonitorState>(std::move(*placement), std::move(control_state),
                                                    control, cancellation,
                                                    static_cast<FactoryState*>(opaque)->platform);
        return RunnerControlMonitor::create_owned(state, monitor_start, monitor_stop);
    } catch (const std::exception&) {
        return std::unexpected{service_error("runtime.service_threads.contained_exception",
                                             "contained_exception", "factory")};
    } catch (...) {
        return std::unexpected{service_error("runtime.service_threads.contained_unknown_failure",
                                             "contained_unknown_failure", "factory")};
    }
}

} // namespace

namespace detail {

auto production_service_platform_operations() noexcept -> ServicePlatformOperations {
    return {.context = nullptr,
            .current_thread_id = production_tid,
            .set_current_thread_affinity = production_set,
            .get_current_thread_affinity = production_get,
            .current_cpu = production_cpu};
}

auto ProductionControlMonitorAccess::startup(const ServiceThreadAssignment& assignment,
                                             const ServicePlatformOperations& platform) noexcept
    -> std::expected<ServiceThreadReadyEvidence, core::Error> {
    try {
        auto tid = platform.current_thread_id(platform.context);
        if (!tid)
            return std::unexpected{service_error("runtime.service_threads.placement_failed",
                                                 "service_identity_failed", "placement",
                                                 &tid.error())};
        const core::CpuSet requested{assignment.linux_cpu};
        auto set = platform.set_current_thread_affinity(platform.context, requested);
        if (!set)
            return std::unexpected{service_error("runtime.service_threads.placement_failed",
                                                 "affinity_apply_failed", "placement",
                                                 &set.error())};
        auto affinity = platform.get_current_thread_affinity(platform.context);
        if (!affinity)
            return std::unexpected{service_error("runtime.service_threads.placement_failed",
                                                 "affinity_readback_failed", "placement",
                                                 &affinity.error())};
        if (*affinity != requested)
            return std::unexpected{service_error("runtime.service_threads.placement_failed",
                                                 "affinity_mismatch", "placement")};
        auto cpu = platform.current_cpu(platform.context);
        if (!cpu)
            return std::unexpected{service_error("runtime.service_threads.placement_failed",
                                                 "current_cpu_failed", "placement", &cpu.error())};
        if (*cpu != assignment.linux_cpu)
            return std::unexpected{service_error("runtime.service_threads.placement_failed",
                                                 "current_cpu_mismatch", "placement")};
        return ServiceThreadReadyEvidence{assignment.role,
                                          assignment.stable_pu,
                                          assignment.stable_core,
                                          assignment.linux_cpu,
                                          *tid,
                                          std::move(*affinity),
                                          *cpu};
    } catch (const std::exception&) {
        return std::unexpected{service_error("runtime.service_threads.contained_exception",
                                             "contained_exception", "placement")};
    } catch (...) {
        return std::unexpected{service_error("runtime.service_threads.contained_unknown_failure",
                                             "contained_unknown_failure", "placement")};
    }
}

auto ProductionControlMonitorAccess::monitor_once(ipc::RunnerControlState& control_state,
                                                  RunnerControlReadCapability& control,
                                                  RunnerCancellation& cancellation,
                                                  std::optional<ipc::CancelReason>& recorded_reason,
                                                  bool& eof) noexcept
    -> std::expected<bool, core::Error> {
    try {
        auto next = control.read_next();
        if (!next)
            return std::unexpected{service_error("runtime.service_threads.control_failed",
                                                 "monitor_read_failed", "control", &next.error())};
        if (std::holds_alternative<RunnerControlLocalStop>(*next))
            return false;
        if (std::holds_alternative<RunnerControlPeerEof>(*next)) {
            eof = true;
            return false;
        }
        const auto& envelope = std::get<ipc::Envelope>(*next);
        auto accepted = control_state.accept(envelope);
        if (!accepted)
            return std::unexpected{service_error("runtime.service_threads.control_failed",
                                                 "monitor_protocol_failed", "control",
                                                 &accepted.error())};
        auto valid = ipc::validate_payload(envelope.type(), envelope.payload());
        if (!valid)
            return std::unexpected{service_error("runtime.service_threads.control_failed",
                                                 "monitor_protocol_failed", "control",
                                                 &valid.error())};
        auto cancel = ipc::parse_cancel_request(envelope);
        if (!cancel)
            return std::unexpected{service_error("runtime.service_threads.control_failed",
                                                 "monitor_protocol_failed", "control",
                                                 &cancel.error())};
        const auto reason = cancel->reason();
        if (!cancellation.try_request(reason) && cancellation.reason() != reason)
            return std::unexpected{service_error("runtime.service_threads.invariant_failed",
                                                 "cancel_publication_mismatch", "control")};
        if (recorded_reason)
            return std::unexpected{service_error("runtime.service_threads.control_failed",
                                                 "monitor_protocol_failed", "control")};
        recorded_reason = reason;
        return true;
    } catch (const std::exception&) {
        return std::unexpected{service_error("runtime.service_threads.contained_exception",
                                             "contained_exception", "control")};
    } catch (...) {
        return std::unexpected{service_error("runtime.service_threads.contained_unknown_failure",
                                             "contained_unknown_failure", "control")};
    }
}

auto ProductionControlMonitorAccess::create(const AdmittedPlacementRequest& admitted,
                                            std::unique_ptr<ipc::RunnerControlState> control_state,
                                            RunnerControlReadCapability& control,
                                            RunnerCancellation& cancellation,
                                            ServicePlatformOperations platform) noexcept
    -> std::expected<RunnerControlMonitor, core::Error> {
    FactoryState state{platform};
    return create_monitor(&state, admitted, std::move(control_state), control, cancellation);
}

} // namespace detail

auto production_runner_control_monitor_factory() noexcept -> RunnerControlMonitorFactory {
    static FactoryState state{detail::production_service_platform_operations()};
    return {&state, create_monitor};
}

auto service_thread_readiness(const RunnerControlMonitor& monitor) noexcept
    -> std::expected<std::reference_wrapper<const ServiceThreadReadyEvidence>, core::Error> {
    try {
        if (!monitor.owned_context_)
            return std::unexpected{service_error("runtime.service_threads.invalid_argument",
                                                 "not_placed_service_monitor", "readiness")};
        auto& state = *static_cast<MonitorState*>(monitor.context_);
        std::lock_guard lock{state.mutex};
        if (!state.readiness)
            return std::unexpected{service_error("runtime.service_threads.invalid_state",
                                                 "service_not_ready", "readiness")};
        return std::cref(*state.readiness);
    } catch (const std::exception&) {
        return std::unexpected{service_error("runtime.service_threads.contained_exception",
                                             "contained_exception", "readiness")};
    } catch (...) {
        return std::unexpected{service_error("runtime.service_threads.contained_unknown_failure",
                                             "contained_unknown_failure", "readiness")};
    }
}

} // namespace epycbench::runtime
