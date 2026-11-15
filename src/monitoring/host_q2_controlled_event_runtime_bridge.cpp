#include "monitoring/host_q2_controlled_event_runtime_bridge.hpp"
#include <limits>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto bridge_error() -> core::Error;
}
struct MonitoringQ2BridgeRetirementRecovery::Impl final {
    std::shared_ptr<MonitoringQ2ControlledEventRuntimeBridge::State> state;
    std::optional<MonitoringQ2ControlledEventSession> controlled;
    Q2TargetOperations operations{};
    std::optional<Q2ControlledFailure> failure;
    std::optional<core::Error> overflow;
    bool overflowed{};
    bool failure_taken{};
    bool armed{};
};
struct MonitoringQ2ControlledEventRuntimeBridge::State {
    enum class QuiescePhase : std::uint8_t { idle, begun, ready, cancelled, quiet };
    MonitoringQ2WorkerSession* owner{};
    std::size_t target_index;
    Q2TargetSnapshot expected;
    std::atomic<std::uint64_t> callback_generation{};
    std::atomic<bool> exact_context{};
    std::atomic<bool> terminal{};
    Q2RuntimeGenerationOperations operations{};
    std::optional<core::Error> readiness_failure;
    Q2ControlledGenerationProtocol* protocol{};
    std::atomic<std::uint64_t> iteration{};
    std::atomic<std::uint64_t> side_effect_sink{};
    MonitoringQ2WaitContextCapsule active_wait;
    QuiescePhase quiesce_phase{QuiescePhase::idle};
    std::unique_ptr<MonitoringQ2BridgeRetirementRecovery::Impl> recovery_carrier;

    bool released{};
    State(MonitoringQ2WorkerSession& value, std::size_t index, Q2TargetSnapshot target)
        : owner{&value}, target_index{index}, expected{std::move(target)},
          recovery_carrier{std::make_unique<MonitoringQ2BridgeRetirementRecovery::Impl>()} {
        recovery_carrier->failure.emplace(bridge_error());
        recovery_carrier->overflow.emplace(bridge_error());
        recovery_carrier->overflow->context["classification"] = "cleanup_overflow";
    }
    State(std::size_t index, Q2TargetSnapshot target, Q2RuntimeGenerationOperations injected)
        : target_index{index}, expected{std::move(target)}, operations{injected},
          recovery_carrier{std::make_unique<MonitoringQ2BridgeRetirementRecovery::Impl>()} {
        recovery_carrier->failure.emplace(bridge_error());
        recovery_carrier->overflow.emplace(bridge_error());
        recovery_carrier->overflow->context["classification"] = "cleanup_overflow";
    }
};
auto run_q2_controlled_target_body(
    const Q2TargetSnapshot& expected, std::size_t target_index, std::atomic<bool>& exact_context,
    std::atomic<bool>& terminal, const runtime::WorkerExecutionContext& context,
    const runtime::detail::TargetedWorkerExecutionControl& control) noexcept
    -> runtime::detail::TargetedWorkerCallbackResult {
    const auto& ready = context.readiness;
    static_cast<void>(terminal);
    static_cast<void>(control);
    const bool exact =
        context.worker_index == target_index && context.worker_id == expected.worker &&
        context.requested_cpu == expected.cpu && context.core == expected.core &&
        context.processing_unit == expected.pu && ready.tid == expected.tid &&
        ready.stable_core == expected.stable_core && ready.stable_pu == expected.stable_pu;
    exact_context.store(exact, std::memory_order_release);
    return {exact ? runtime::detail::TargetedWorkerDisposition::succeeded
                  : runtime::detail::TargetedWorkerDisposition::caller_failure,
            exact ? 1U : 0U};
}
void run_q2_target_event_loop(
    Q2ControlledGenerationProtocol& protocol, std::uint64_t generation,
    std::atomic<std::uint64_t>& iteration, std::atomic<std::uint64_t>& side_effect_sink,
    const runtime::detail::TargetedWorkerExecutionControl& control) noexcept {
    protocol.target_running.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.target_running);
    while (!protocol.stop.load(std::memory_order_acquire) && !control.cancellation_requested() &&
           !control.team_stop.stop_requested() && !control.command_stop.stop_requested()) {
        const auto value = iteration.load(std::memory_order_relaxed);
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            auto expected = Q2Winner::undecided;
            static_cast<void>(protocol.winner.compare_exchange_strong(
                expected, Q2Winner::ceiling_first, std::memory_order_acq_rel));
            break;
        }
        side_effect_sink.fetch_xor((value + 1U) * 0x9e3779b97f4a7c15ULL, std::memory_order_relaxed);
        iteration.store(value + 1U, std::memory_order_relaxed);
    }
    control.signal_timed_completion();
    protocol.target_terminal.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.target_terminal);
}
auto run_q2_voluntary_readiness_body(const Q2TargetSnapshot& expected, std::size_t target_index,
                                     Q2ControlledGenerationProtocol& protocol,
                                     std::uint64_t generation,
                                     const runtime::WorkerExecutionContext& context) noexcept
    -> bool {
    const auto& ready = context.readiness;
    const bool exact =
        context.worker_index == target_index && context.worker_id == expected.worker &&
        context.requested_cpu == expected.cpu && context.core == expected.core &&
        context.processing_unit == expected.pu && ready.tid == expected.tid &&
        ready.stable_core == expected.stable_core && ready.stable_pu == expected.stable_pu;
    if (exact)
        protocol.voluntary_ready.store(generation, std::memory_order_release);
    if (exact)
        monitoring_q2_atomic_wake(protocol.voluntary_ready);
    return exact;
}
auto run_q2_voluntary_loop_body(Q2ControlledGenerationProtocol& protocol, std::uint64_t generation,
                                const runtime::detail::TargetedWorkerExecutionControl& control,
                                MonitoringQ2DeadlineCancellationContext wait) noexcept -> bool {
    protocol.voluntary_parked.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.voluntary_parked);
    while (protocol.voluntary_command.load(std::memory_order_acquire) == 0U &&
           !control.cancellation_requested() && !control.team_stop.stop_requested() &&
           !control.command_stop.stop_requested()) {
        auto waited = monitoring_q2_bounded_atomic_wait(protocol.voluntary_command, 0U, wait);
        if (!waited)
            return false;
    }
    protocol.voluntary_resumed.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.voluntary_resumed);
    protocol.voluntary_terminal.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.voluntary_terminal);
    control.signal_timed_completion();
    return true;
}
namespace {
auto exact_target(const Q2TargetSnapshot& expected, std::size_t target_index,
                  const runtime::WorkerExecutionContext& context) noexcept -> bool {
    const auto& ready = context.readiness;
    return context.worker_index == target_index && context.worker_id == expected.worker &&
           context.requested_cpu == expected.cpu && context.core == expected.core &&
           context.processing_unit == expected.pu && ready.tid == expected.tid &&
           ready.stable_core == expected.stable_core && ready.stable_pu == expected.stable_pu;
}
auto target_readiness(void* raw, const runtime::WorkerExecutionContext& context) noexcept
    -> std::expected<void, core::Error> {
    auto& state = *static_cast<MonitoringQ2ControlledEventRuntimeBridge::State*>(raw);
    const bool exact = exact_target(state.expected, state.target_index, context);
    state.exact_context.store(exact, std::memory_order_release);
    if (exact && state.protocol)
        state.protocol->target_ready.store(
            state.callback_generation.load(std::memory_order_acquire), std::memory_order_release);
    if (exact && state.protocol)
        monitoring_q2_atomic_wake(state.protocol->target_ready);
    if (!exact)
        return std::unexpected{std::move(*state.readiness_failure)};
    return {};
}
auto revalidation_readiness(void* raw, const runtime::WorkerExecutionContext& context) noexcept
    -> std::expected<void, core::Error> {
    auto& state = *static_cast<MonitoringQ2ControlledEventRuntimeBridge::State*>(raw);
    const bool exact = exact_target(state.expected, state.target_index, context);
    state.exact_context.store(exact, std::memory_order_release);
    if (!exact)
        return std::unexpected{std::move(*state.readiness_failure)};
    return {};
}
auto target_loop(void* raw, const runtime::WorkerExecutionContext& context,
                 const runtime::detail::TargetedWorkerExecutionControl& control) noexcept
    -> runtime::detail::TargetedWorkerCallbackResult {
    auto& state = *static_cast<MonitoringQ2ControlledEventRuntimeBridge::State*>(raw);
    auto result = run_q2_controlled_target_body(
        state.expected, state.target_index, state.exact_context, state.terminal, context, control);
    if (state.protocol &&
        result.disposition == runtime::detail::TargetedWorkerDisposition::succeeded &&
        state.exact_context.load(std::memory_order_acquire)) {
        const auto generation = state.callback_generation.load(std::memory_order_acquire);
        run_q2_target_event_loop(*state.protocol, generation, state.iteration,
                                 state.side_effect_sink, control);
        state.terminal.store(true, std::memory_order_release);
    } else if (state.protocol) {
        state.protocol->target_terminal.store(
            state.callback_generation.load(std::memory_order_acquire), std::memory_order_release);
        monitoring_q2_atomic_wake(state.protocol->target_terminal);
        state.terminal.store(true, std::memory_order_release);
    }
    return result;
}
auto revalidation_loop(void* raw, const runtime::WorkerExecutionContext& context,
                       const runtime::detail::TargetedWorkerExecutionControl& control) noexcept
    -> runtime::detail::TargetedWorkerCallbackResult {
    auto& state = *static_cast<MonitoringQ2ControlledEventRuntimeBridge::State*>(raw);
    return run_q2_controlled_target_body(state.expected, state.target_index, state.exact_context,
                                         state.terminal, context, control);
}
auto voluntary_readiness(void* raw, const runtime::WorkerExecutionContext& context) noexcept
    -> std::expected<void, core::Error> {
    auto& state = *static_cast<MonitoringQ2ControlledEventRuntimeBridge::State*>(raw);
    if (!run_q2_voluntary_readiness_body(state.expected, state.target_index, *state.protocol,
                                         state.callback_generation.load(), context))
        return std::unexpected{std::move(*state.readiness_failure)};
    state.exact_context.store(true, std::memory_order_release);
    return {};
}
auto voluntary_loop(void* raw, const runtime::WorkerExecutionContext&,
                    const runtime::detail::TargetedWorkerExecutionControl& control) noexcept
    -> runtime::detail::TargetedWorkerCallbackResult {
    auto& state = *static_cast<MonitoringQ2ControlledEventRuntimeBridge::State*>(raw);
    const auto generation = state.callback_generation.load(std::memory_order_acquire);
    const bool completed =
        run_q2_voluntary_loop_body(*state.protocol, generation, control, state.active_wait.load());
    state.terminal.store(true, std::memory_order_release);
    return {completed ? runtime::detail::TargetedWorkerDisposition::succeeded
                      : runtime::detail::TargetedWorkerDisposition::caller_failure,
            completed ? 1U : 0U};
}
auto bridge_error() -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.controlled_event.runtime_bridge"},
            .message = "Q2 controlled target bridge rejected",
            .operation = "monitoring_q2_controlled_event_runtime_bridge",
            .os_error = std::nullopt,
            .context = {},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto valid(const Q2RuntimeGenerationOperations& operations) noexcept -> bool {
    return operations.begin && operations.await_ready && operations.release &&
           operations.await_timed_completion && operations.await_quiescence &&
           operations.collect_succeeded && operations.cancel_ready && operations.reset;
}
} // namespace
MonitoringQ2QuiescedWorkerBorrow::MonitoringQ2QuiescedWorkerBorrow(
    MonitoringQ2QuiescedWorkerBorrow&& other) noexcept
    : owner_{other.owner_}, index_{other.index_}, expected_{std::move(other.expected_)},
      operations_{other.operations_}, lifetime_{std::move(other.lifetime_)},
      consumed_{std::exchange(other.consumed_, true)} {
    other.owner_ = nullptr;
}
MonitoringQ2QuiescedWorkerBorrow::~MonitoringQ2QuiescedWorkerBorrow() noexcept {
    if (owner_ && !consumed_)
        std::terminate();
}
MonitoringQ2BridgeRetirementRecovery::MonitoringQ2BridgeRetirementRecovery(
    std::unique_ptr<Impl> value) noexcept
    : impl_{std::move(value)} {}
MonitoringQ2BridgeRetirementRecovery::MonitoringQ2BridgeRetirementRecovery(
    MonitoringQ2BridgeRetirementRecovery&&) noexcept = default;
MonitoringQ2BridgeRetirementRecovery::~MonitoringQ2BridgeRetirementRecovery() noexcept {
    if (impl_ && impl_->armed)
        std::terminate();
}
auto MonitoringQ2BridgeRetirementRecovery::failure() const noexcept -> const Q2ControlledFailure& {
    if (!impl_ || !impl_->failure || impl_->failure_taken)
        std::terminate();
    return *impl_->failure;
}
auto MonitoringQ2BridgeRetirementRecovery::retry(
    MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::optional<MonitoringQ2RecoveredWorkerFailure> {
    if (!impl_ || !impl_->armed || !impl_->controlled || impl_->failure_taken)
        std::terminate();
    auto retired = impl_->controlled->retire(wait);
    if (!retired) {
        auto next = std::move(retired.error());
        const auto append = [&](core::Error value) {
            if (impl_->overflowed)
                return;
            if (impl_->failure->cleanup_count + 1U < impl_->failure->cleanup.size()) {
                impl_->failure->cleanup[impl_->failure->cleanup_count++].emplace(std::move(value));
                return;
            }
            impl_->failure->cleanup.back().emplace(std::move(*impl_->overflow));
            impl_->failure->cleanup_count = impl_->failure->cleanup.size();
            impl_->overflowed = true;
        };
        if (next.has_primary)
            append(std::move(next.primary));
        for (std::size_t index{}; index < next.cleanup_count; ++index)
            append(std::move(*next.cleanup[index]));
        for (std::size_t index{}; index < next.compact_cleanup.size(); ++index)
            impl_->failure->compact_cleanup.append(next.compact_cleanup[index]);
        if (next.compact_cleanup.overflowed())
            impl_->failure->compact_cleanup.note_overflow();
        return std::nullopt;
    }
    auto& state = *impl_->state;
    MonitoringQ2RecoveredWorkerFailure result{
        MonitoringQ2QuiescedWorkerBorrow{state.owner, state.target_index, state.expected,
                                         impl_->operations, impl_->state},
        std::move(*impl_->failure)};
    state.released = true;
    impl_->controlled.reset();
    impl_->armed = false;
    return result;
}
auto MonitoringQ2BridgeRetirementRecovery::take_failure_for_containment() noexcept
    -> Q2ControlledFailure {
    if (!impl_ || !impl_->armed || !impl_->failure || impl_->failure_taken)
        std::terminate();
    impl_->failure_taken = true;
    return std::move(*impl_->failure);
}
auto MonitoringQ2ControlledEventRuntimeBridge::create(
    MonitoringQ2WorkerSession& workers, std::size_t index,
    const MonitoringQ2WorkerBinding& binding) noexcept
    -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error> {
    bool borrowed = false;
    try {
        static_cast<void>(workers.borrow(index));
        borrowed = true;
        if (!workers.native_runtime_) {
            workers.release(index, false);
            return std::unexpected{bridge_error()};
        }
        auto ready = workers.native_runtime_->readiness(index);
        if (!ready) {
            workers.release(index, false);
            borrowed = false;
            return std::unexpected{std::move(ready.error())};
        }
        Q2TargetSnapshot target{
            ready->worker_id,   ready->requested_cpu, binding.core, binding.processing_unit,
            ready->stable_core, ready->stable_pu,     ready->tid,   true};
        if (target.worker != binding.worker || target.cpu != binding.linux_cpu ||
            target.stable_core != binding.stable_core ||
            target.stable_pu != binding.stable_processing_unit) {
            workers.release(index, false);
            borrowed = false;
            return std::unexpected{bridge_error()};
        }
        auto state = std::make_shared<State>(workers, index, std::move(target));
        state->operations = workers.native_runtime_->generation_operations();
        state->readiness_failure.emplace(bridge_error());
        return MonitoringQ2ControlledEventRuntimeBridge{std::move(state)};
    } catch (...) {
        if (borrowed)
            workers.release(index, false);
        return std::unexpected{bridge_error()};
    }
}
auto MonitoringQ2ControlledEventRuntimeBridge::adopt_existing_borrow(
    MonitoringQ2SourcePreparationWorkerHandoff&& handoff, void* test_context,
    void (*before_allocation_test)(void*)) noexcept
    -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error> {
    if (handoff.consumed_ || !handoff.owner_ || handoff.page_recheck_ ||
        !handoff.owner_->borrowed_ || handoff.owner_->next_worker_ != handoff.index_ ||
        !valid(handoff.runtime_operations_))
        std::terminate();
    try {
        if (before_allocation_test)
            before_allocation_test(test_context);
        auto state = std::make_shared<State>(*handoff.owner_, handoff.index_, handoff.target_);
        state->operations = handoff.runtime_operations_;
        state->readiness_failure.emplace(bridge_error());
        handoff.consumed_ = true;
        return MonitoringQ2ControlledEventRuntimeBridge{std::move(state)};
    } catch (...) {
        return std::unexpected{bridge_error()};
    }
}
auto MonitoringQ2ControlledEventRuntimeBridge::create_injected(
    std::size_t index, Q2TargetSnapshot target, Q2RuntimeGenerationOperations operations) noexcept
    -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error> {
    if (!valid(operations))
        return std::unexpected{bridge_error()};
    try {
        auto state = std::make_shared<State>(index, std::move(target), operations);
        state->readiness_failure.emplace(bridge_error());
        return MonitoringQ2ControlledEventRuntimeBridge{std::move(state)};
    } catch (...) {
        return std::unexpected{bridge_error()};
    }
}
auto MonitoringQ2ControlledEventRuntimeBridge::create_borrow_injected(
    MonitoringQ2WorkerSession& owner, std::size_t index, Q2TargetSnapshot target,
    Q2RuntimeGenerationOperations operations) noexcept
    -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error> {
    if (!valid(operations))
        return std::unexpected{bridge_error()};
    static_cast<void>(owner.borrow(index));
    try {
        auto state = std::make_shared<State>(owner, index, std::move(target));
        state->operations = operations;
        state->readiness_failure.emplace(bridge_error());
        return MonitoringQ2ControlledEventRuntimeBridge{std::move(state)};
    } catch (...) {
        owner.release(index, false);
        return std::unexpected{bridge_error()};
    }
}
MonitoringQ2ControlledEventRuntimeBridge::MonitoringQ2ControlledEventRuntimeBridge(
    std::shared_ptr<State> state) noexcept
    : state_{std::move(state)} {}
MonitoringQ2ControlledEventRuntimeBridge::MonitoringQ2ControlledEventRuntimeBridge(
    MonitoringQ2ControlledEventRuntimeBridge&&) noexcept = default;
MonitoringQ2ControlledEventRuntimeBridge::~MonitoringQ2ControlledEventRuntimeBridge() noexcept {
    if (state_ && state_->owner && !state_->released)
        std::terminate();
}
auto MonitoringQ2ControlledEventRuntimeBridge::begin(
    std::uint64_t generation, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2LifecycleResult {
    if (!state_ || generation == 0 || !valid(state_->operations))
        std::terminate();
    state_->callback_generation.store(generation, std::memory_order_release);
    state_->exact_context.store(false, std::memory_order_relaxed);
    state_->terminal.store(false, std::memory_order_relaxed);
    try {
        return state_->operations.begin(state_->operations.context, state_->target_index,
                                        {state_.get(), target_loop},
                                        {state_.get(), target_readiness}, wait);
    } catch (...) {
        return std::unexpected{bridge_error()};
    }
}
auto MonitoringQ2ControlledEventRuntimeBridge::retire(
    MonitoringQ2ControlledEventSession&& controlled,
    MonitoringQ2DeadlineCancellationContext wait) noexcept -> MonitoringQ2BridgeRetirementOutcome {
    if (!state_ || !state_->owner || state_->released || !valid(state_->operations))
        std::terminate();
    // Reserve all recovery ownership before the first teardown transition. Once retirement
    // starts, every failure path below consists only of noexcept ownership moves.
    auto recovery = std::move(state_->recovery_carrier);
    if (!recovery)
        std::terminate();
    recovery->state = state_;
    recovery->controlled.emplace(std::move(controlled));
    recovery->operations = target_operations();
    recovery->armed = true;
    if (!recovery->controlled->retirement_retryable()) {
        recovery->controlled.reset();
        recovery->armed = false;
        auto handoff = MonitoringQ2QuiescedWorkerBorrow{
            state_->owner, state_->target_index, state_->expected, recovery->operations, state_};
        state_->released = true;
        return handoff;
    }
    auto result = recovery->controlled->retire(wait);
    if (!result) {
        recovery->failure = std::move(result.error());
        if (!recovery->controlled->retirement_retryable()) {
            auto failure = std::move(*recovery->failure);
            recovery->controlled.reset();
            recovery->armed = false;
            auto handoff =
                MonitoringQ2QuiescedWorkerBorrow{state_->owner, state_->target_index,
                                                 state_->expected, recovery->operations, state_};
            state_->released = true;
            return MonitoringQ2RecoveredWorkerFailure{std::move(handoff), std::move(failure)};
        }
        state_.reset();
        return MonitoringQ2BridgeRetirementRecovery{std::move(recovery)};
    }
    recovery->controlled.reset();
    recovery->armed = false;
    auto handoff = MonitoringQ2QuiescedWorkerBorrow{state_->owner, state_->target_index,
                                                    state_->expected, target_operations(), state_};
    state_->released = true;
    return MonitoringQ2BridgeRetirementOutcome{std::in_place_index<0>, std::move(handoff)};
}
auto MonitoringQ2ControlledEventRuntimeBridge::retire_before_controlled_session() noexcept
    -> MonitoringQ2QuiescedWorkerBorrow {
    if (!state_ || !state_->owner || state_->released || !valid(state_->operations))
        std::terminate();
    auto handoff = MonitoringQ2QuiescedWorkerBorrow{state_->owner, state_->target_index,
                                                    state_->expected, target_operations(), state_};
    state_->released = true;
    return handoff;
}
auto MonitoringQ2ControlledEventRuntimeBridge::await_ready(
    std::uint64_t generation, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2LifecycleResult {
    if (!state_ || state_->callback_generation.load(std::memory_order_acquire) != generation)
        std::terminate();
    return state_->operations.await_ready(state_->operations.context, wait);
}
auto MonitoringQ2ControlledEventRuntimeBridge::release(std::uint64_t generation) noexcept
    -> Q2LifecycleResult {
    if (!state_ || state_->callback_generation.load(std::memory_order_acquire) != generation)
        std::terminate();
    return state_->operations.release(state_->operations.context);
}
auto MonitoringQ2ControlledEventRuntimeBridge::drain_terminal(
    std::uint64_t generation, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2LifecycleResult {
    if (!state_ || state_->callback_generation.load(std::memory_order_acquire) != generation)
        std::terminate();
    auto timed = state_->operations.await_timed_completion(state_->operations.context, wait);
    if (!timed)
        return timed;
    auto quiet = state_->operations.await_quiescence(state_->operations.context, wait);
    if (!quiet)
        return quiet;
    auto collected = state_->operations.collect_succeeded(state_->operations.context);
    if (!collected)
        return std::unexpected{std::move(collected.error())};
    return *collected ? Q2LifecycleResult{} : Q2LifecycleResult{std::unexpected{bridge_error()}};
}
auto MonitoringQ2ControlledEventRuntimeBridge::repark(
    std::uint64_t generation, MonitoringQ2DeadlineCancellationContext) noexcept
    -> Q2LifecycleResult {
    if (!state_ || state_->callback_generation.load(std::memory_order_acquire) != generation)
        return std::unexpected{bridge_error()};
    return state_->operations.reset(state_->operations.context);
}
auto MonitoringQ2ControlledEventRuntimeBridge::snapshot() const noexcept
    -> const Q2TargetSnapshot* {
    return state_ ? &state_->expected : nullptr;
}
void MonitoringQ2ControlledEventRuntimeBridge::set_iteration_for_test(
    std::uint64_t value) noexcept {
    if (!state_)
        std::terminate();
    state_->iteration.store(value, std::memory_order_relaxed);
}
auto MonitoringQ2ControlledEventRuntimeBridge::target_operations() noexcept -> Q2TargetOperations {
    if (!state_)
        std::terminate();
    return {
        state_.get(),
        [](void* raw, std::size_t index) noexcept -> const Q2TargetSnapshot* {
            auto& state = *static_cast<State*>(raw);
            return index == state.target_index ? &state.expected : nullptr;
        },
        [](void* raw, Q2ControlledGenerationProtocol* protocol) noexcept {
            static_cast<State*>(raw)->protocol = protocol;
        },
        [](void* raw, std::size_t index,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || !valid(state.operations))
                return std::unexpected{bridge_error()};
            if (state.quiesce_phase == State::QuiescePhase::idle) {
                auto begun = state.operations.begin(state.operations.context, index,
                                                    {&state, revalidation_loop},
                                                    {&state, revalidation_readiness}, wait);
                if (!begun)
                    return begun;
                state.quiesce_phase = State::QuiescePhase::begun;
            }
            if (state.quiesce_phase == State::QuiescePhase::begun) {
                auto ready = state.operations.await_ready(state.operations.context, wait);
                if (!ready)
                    return ready;
                state.quiesce_phase = State::QuiescePhase::ready;
            }
            if (state.quiesce_phase == State::QuiescePhase::ready) {
                auto cancelled = state.operations.cancel_ready(state.operations.context);
                if (!cancelled)
                    return cancelled;
                state.quiesce_phase = State::QuiescePhase::cancelled;
            }
            if (state.quiesce_phase == State::QuiescePhase::cancelled) {
                auto quiet = state.operations.await_quiescence(state.operations.context, wait);
                if (!quiet)
                    return quiet;
                state.quiesce_phase = State::QuiescePhase::quiet;
            }
            auto reset = state.operations.reset(state.operations.context);
            if (reset)
                state.quiesce_phase = State::QuiescePhase::idle;
            return reset;
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || generation == 0 || !valid(state.operations))
                return std::unexpected{bridge_error()};
            state.callback_generation.store(generation, std::memory_order_release);
            state.exact_context.store(false, std::memory_order_relaxed);
            state.terminal.store(false, std::memory_order_relaxed);
            auto begun =
                state.operations.begin(state.operations.context, state.target_index,
                                       {&state, target_loop}, {&state, target_readiness}, wait);
            if (!begun)
                return begun;
            return state.operations.await_ready(state.operations.context, wait);
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index ||
                state.callback_generation.load(std::memory_order_acquire) != generation)
                return std::unexpected{bridge_error()};
            return state.operations.release(state.operations.context);
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || !state.protocol ||
                state.callback_generation.load(std::memory_order_acquire) != generation)
                return std::unexpected{bridge_error()};
            state.protocol->stop.store(true, std::memory_order_release);
            return {};
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index ||
                state.callback_generation.load(std::memory_order_acquire) != generation)
                return std::unexpected{bridge_error()};
            auto timed = state.operations.await_timed_completion(state.operations.context, wait);
            if (!timed)
                return timed;
            auto quiet = state.operations.await_quiescence(state.operations.context, wait);
            if (!quiet)
                return quiet;
            auto collected = state.operations.collect_succeeded(state.operations.context);
            if (!collected)
                return std::unexpected{std::move(collected.error())};
            return *collected ? Q2LifecycleResult{}
                              : Q2LifecycleResult{std::unexpected{bridge_error()}};
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index ||
                state.callback_generation.load(std::memory_order_acquire) != generation)
                return std::unexpected{bridge_error()};
            return state.operations.reset(state.operations.context);
        },
        [](void* raw, std::size_t index,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || !valid(state.operations))
                return std::unexpected{bridge_error()};
            return {};
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || generation == 0 || !valid(state.operations))
                return std::unexpected{bridge_error()};
            state.callback_generation.store(generation, std::memory_order_release);
            state.exact_context.store(false, std::memory_order_relaxed);
            state.terminal.store(false, std::memory_order_relaxed);
            state.active_wait.publish(wait);
            state.protocol->voluntary_command.store(0U, std::memory_order_release);
            auto begun =
                state.operations.begin(state.operations.context, index, {&state, voluntary_loop},
                                       {&state, voluntary_readiness}, wait);
            if (!begun)
                return begun;
            return state.operations.await_ready(state.operations.context, wait);
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || state.callback_generation.load() != generation)
                return std::unexpected{bridge_error()};
            auto released = state.operations.release(state.operations.context);
            if (!released)
                return released;
            for (;;) {
                const auto observed =
                    state.protocol->voluntary_parked.load(std::memory_order_acquire);
                if (observed == generation)
                    break;
                auto waited = monitoring_q2_bounded_atomic_wait(state.protocol->voluntary_parked,
                                                                observed, wait);
                if (!waited)
                    return waited;
            }
            return {};
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || state.callback_generation.load() != generation)
                return std::unexpected{bridge_error()};
            state.protocol->voluntary_command.store(1U, std::memory_order_release);
            monitoring_q2_atomic_wake(state.protocol->voluntary_command);
            return {};
        },
        [](void* raw, std::size_t index, std::uint64_t generation,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& state = *static_cast<State*>(raw);
            if (index != state.target_index || state.callback_generation.load() != generation)
                return std::unexpected{bridge_error()};
            auto timed = state.operations.await_timed_completion(state.operations.context, wait);
            if (!timed)
                return timed;
            auto quiet = state.operations.await_quiescence(state.operations.context, wait);
            if (!quiet)
                return quiet;
            auto result = state.operations.collect_succeeded(state.operations.context);
            if (!result)
                return std::unexpected{std::move(result.error())};
            if (!*result || !state.exact_context.load(std::memory_order_acquire) ||
                !state.terminal.load(std::memory_order_acquire))
                return std::unexpected{bridge_error()};
            return state.operations.reset(state.operations.context);
        }};
}
} // namespace epycbench::monitoring::qualification::detail
