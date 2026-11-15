#include "monitoring/host_q2_controlled_event_session.hpp"
#include <utility>
namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ2ControlledEventSession::LoopState {
    Q2HelperOperations operations{};
    std::atomic<std::uint32_t> command{};
    std::atomic<std::uint64_t> requested_generation{};
    std::atomic<std::uint64_t> ready_generation{};
    std::atomic<std::uint64_t> running_generation{};
    std::atomic<std::uint64_t> terminal_generation{};
    std::atomic<std::uint64_t> park_offer{};
    std::atomic<std::uint64_t> parked_generation{};
    std::uint64_t completed_generation{};
    Q2ControlledGenerationProtocol protocol;
    MonitoringQ2WaitContextCapsule command_wait;
    void request_stop() noexcept {
        // The real helper body observes this shared stop, not its owner's native
        // stop token. Publish it before stopping either the body or its park wait.
        protocol.stop.store(true, std::memory_order_release);
        command.store(3U, std::memory_order_release);
        // All nonzero increasing uint64 generations remain accepted, including MAX.
        // Select from three values to change the wait word without offering either
        // its old value or the active generation; no new generation is admitted.
        const auto previous = park_offer.load(std::memory_order_acquire);
        const auto generation = requested_generation.load(std::memory_order_acquire);
        const std::uint64_t stopped = previous != 0U && generation != 0U ? 0U
                                       : previous != 1U && generation != 1U ? 1U : 2U;
        park_offer.store(stopped, std::memory_order_release);
        operations.wake(operations.context, &command);
        monitoring_q2_atomic_wake(park_offer);
    }
};
void run_q2_helper_event_loop(Q2ControlledGenerationProtocol& protocol,
                              std::uint64_t generation) noexcept {
    auto expected = Q2Winner::undecided;
    static_cast<void>(protocol.winner.compare_exchange_strong(expected, Q2Winner::helper_first,
                                                              std::memory_order_acq_rel));
    // Publishing running is the controller's release/compete observation boundary.  The winner
    // claim must happen-before it; otherwise a waiter can observe running and read undecided.
    protocol.helper_running.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.helper_running);
    while (!protocol.stop.load(std::memory_order_acquire)) {
        const auto value = protocol.helper_iteration.load(std::memory_order_relaxed);
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            protocol.helper_ceiling.store(generation, std::memory_order_release);
            break;
        }
        protocol.helper_side_effect_sink.fetch_xor((value + 1U) * 0xd6e8feb86659fd93ULL,
                                                   std::memory_order_relaxed);
        protocol.helper_iteration.store(value + 1U, std::memory_order_relaxed);
        if (value + 1U >= protocol.helper_test_stop_at.load(std::memory_order_relaxed))
            protocol.stop.store(true, std::memory_order_release);
    }
    protocol.helper_terminal.store(generation, std::memory_order_release);
    monitoring_q2_atomic_wake(protocol.helper_terminal);
}
namespace {
constexpr std::uint32_t readiness_requested = 4U;
thread_local MonitoringQ2ControlledEventSessionTestAccess::PublicationHook publication_hook;
thread_local MonitoringQ2ControlledEventSessionTestAccess::HelperWaitHook helper_wait_hook;
auto diagnostic(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.controlled_event.invalid"},
            .message = "Q2 controlled event rejected",
            .operation = "monitoring_q2_controlled_event",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto wait_status(std::atomic<std::uint64_t>& word, std::uint64_t generation,
                 MonitoringQ2DeadlineCancellationContext wait,
                 std::optional<core::Error>& pending) noexcept -> Q2EventStatus {
    for (;;) {
        const auto observed = word.load(std::memory_order_acquire);
        if (observed == generation)
            break;
        auto result = monitoring_q2_bounded_atomic_wait(word, observed, wait);
        if (!result) {
            const auto classification = result.error().context.value("classification", "");
            pending.emplace(std::move(result.error()));
            if (classification == "timeout")
                return Q2EventStatus::deadline;
            if (classification == "cancelled")
                return Q2EventStatus::cancelled;
            return Q2EventStatus::helper_failure;
        }
    }
    return Q2EventStatus::ok;
}
void append(Q2ControlledFailure& f, core::Error e, core::Error& overflow) noexcept {
    if (f.cleanup_overflowed)
        return;
    if (f.cleanup_count + 1U < f.cleanup.size())
        f.cleanup[f.cleanup_count++].emplace(std::move(e));
    else {
        f.cleanup.back().emplace(std::move(overflow));
        f.cleanup_count = f.cleanup.size();
        f.cleanup_overflowed = true;
    }
}
void append(Q2ControlledFailure& f, MonitoringQ2CompactDiagnostic value) noexcept {
    f.compact_cleanup.append(value);
}
} // namespace
void MonitoringQ2ControlledEventSessionTestAccess::install_publication_hook(
    PublicationHook hook) noexcept { publication_hook = hook; }
void MonitoringQ2ControlledEventSessionTestAccess::install_helper_wait_hook(
    HelperWaitHook hook) noexcept { helper_wait_hook = hook; }
MonitoringQ2ControlledEventSession::MonitoringQ2ControlledEventSession(
    Q2TargetSnapshot t, std::size_t index, Q2TargetOperations to, Q2HelperOperations ho,
    Q2HelperOperations::Handle h, core::Error overflow, core::Error binding,
    std::unique_ptr<LoopState> loop) noexcept
    : target_{t}, target_ops_{to}, helper_ops_{ho}, helper_{h},
      overflow_error_{std::move(overflow)}, binding_error_{std::move(binding)},
      target_index_{index}, loop_{std::move(loop)} {}
void MonitoringQ2ControlledEventSession::helper_entry(void* raw) noexcept {
    auto& loop = *static_cast<LoopState*>(raw);
    for (;;) {
        const auto command = loop.command.load(std::memory_order_acquire);
        if (command == 3U)
            return;
        if (command == readiness_requested) {
            // Acquiring the request observes its generation and every protocol reset.
            const auto generation = loop.requested_generation.load(std::memory_order_acquire);
            auto expected = readiness_requested;
            if (!loop.command.compare_exchange_strong(expected, 0U, std::memory_order_acq_rel))
                continue;
            // Consume the request before readiness allows the controller to publish run=1.
            if (generation != 0U && generation > loop.completed_generation &&
                loop.ready_generation.load(std::memory_order_acquire) != generation) {
                loop.ready_generation.store(generation, std::memory_order_release);
                loop.protocol.helper_ready.store(generation, std::memory_order_release);
                monitoring_q2_atomic_wake(loop.protocol.helper_ready);
            }
            continue;
        }
        const auto generation = loop.requested_generation.load(std::memory_order_acquire);
        if (command == 1U && generation != 0U && generation > loop.completed_generation) {
            run_q2_helper_event_loop(loop.protocol, generation);
            loop.completed_generation = generation;
            for (;;) {
                const auto observed = loop.park_offer.load(std::memory_order_acquire);
                // Read the owned stop after the offer: observing a retirement offer
                // must not authorize a park or enter the old generation's wait.
                if (loop.command.load(std::memory_order_acquire) == 3U)
                    return;
                if (observed == generation)
                    break;
                if (helper_wait_hook)
                    helper_wait_hook(&loop, loop.command.load(std::memory_order_acquire));
                auto offered = monitoring_q2_bounded_atomic_wait(loop.park_offer, observed,
                                                                 loop.command_wait.load());
                if (!offered)
                    return;
            }
            auto pending = loop.command.load(std::memory_order_acquire);
            while (pending != 3U && !loop.command.compare_exchange_strong(
                       pending, 0U, std::memory_order_acq_rel)) {}
            if (pending == 3U)
                return;
            loop.parked_generation.store(generation, std::memory_order_release);
            monitoring_q2_atomic_wake(loop.parked_generation);
            continue;
        }
        // Wait only on the command already examined. A newly published request/run/shutdown
        // differs from this predicate, including when its wake precedes the actual futex wait.
        if (helper_wait_hook)
            helper_wait_hook(&loop, command);
        auto waited = loop.operations.wait(loop.operations.context, &loop.command, command,
                                           loop.command_wait.load());
        if (!waited)
            return;
    }
}
void MonitoringQ2ControlledEventSessionTestAccess::pump_helper(void* raw) noexcept {
    auto& loop = *static_cast<MonitoringQ2ControlledEventSession::LoopState*>(raw);
    const auto requested = loop.requested_generation.load(std::memory_order_acquire);
    const auto command = loop.command.load(std::memory_order_acquire);
    if (command == 3U) {
        if (requested != 0)
            loop.protocol.helper_terminal.store(requested, std::memory_order_release);
        if (requested != 0)
            monitoring_q2_atomic_wake(loop.protocol.helper_terminal);
        return;
    }
    if (requested == 0 || requested <= loop.completed_generation)
        return;
    if (loop.ready_generation.load(std::memory_order_acquire) != requested) {
        loop.ready_generation.store(requested, std::memory_order_release);
        loop.protocol.helper_ready.store(requested, std::memory_order_release);
        monitoring_q2_atomic_wake(loop.protocol.helper_ready);
        return;
    }
    if (command == 1U && loop.running_generation.load(std::memory_order_acquire) != requested) {
        loop.running_generation.store(requested, std::memory_order_release);
        loop.protocol.helper_running.store(requested, std::memory_order_release);
        monitoring_q2_atomic_wake(loop.protocol.helper_running);
        return;
    }
    if (command == 2U) {
        loop.terminal_generation.store(requested, std::memory_order_release);
        loop.protocol.helper_terminal.store(requested, std::memory_order_release);
        monitoring_q2_atomic_wake(loop.protocol.helper_terminal);
        loop.completed_generation = requested;
        loop.command.store(0U, std::memory_order_release);
        loop.parked_generation.store(requested, std::memory_order_release);
    }
}
void MonitoringQ2ControlledEventSessionTestAccess::run_helper_once(void* raw) noexcept {
    auto& loop = *static_cast<MonitoringQ2ControlledEventSession::LoopState*>(raw);
    const auto generation = loop.requested_generation.load(std::memory_order_acquire);
    loop.protocol.stop.store(true, std::memory_order_release);
    run_q2_helper_event_loop(loop.protocol, generation);
    loop.protocol.stop.store(false, std::memory_order_release);
}
auto MonitoringQ2ControlledEventSession::create(std::span<const MonitoringQ2WorkerBinding> w,
                                                std::size_t i, Q2TargetOperations to,
                                                Q2HelperOperations ho) noexcept
    -> std::expected<MonitoringQ2ControlledEventSession, Q2ControlledFailure> {
    std::optional<core::Error> invalid, missing, crossed, overflow;
    try {
        invalid.emplace(diagnostic("operations"));
        missing.emplace(diagnostic("target_missing"));
        crossed.emplace(diagnostic("target_binding"));
        overflow.emplace(diagnostic("cleanup_overflow"));
    } catch (...) {
        core::Error emergency{};
        emergency.category = core::ErrorCategory::monitoring;
        return std::unexpected{Q2ControlledFailure{std::move(emergency)}};
    }
    if (i >= w.size() || !to.snapshot || !to.attach_protocol || !to.begin || !to.release ||
        !to.drain_terminal || !to.repark || !to.quiesce || !ho.launch || !ho.wait || !ho.wake ||
        !ho.bind_cpu || !ho.verify_other_affinity || !ho.registry_enter || !ho.request_stop ||
        !ho.unblock || !ho.drain_terminal || !ho.join || !ho.close || !ho.verify_terminal ||
        !ho.registry_absent || !ho.release_start || !to.stop || !to.revalidate ||
        !to.begin_voluntary || !to.release_to_wait || !to.wake_voluntary || !to.finish_voluntary)
        return std::unexpected{Q2ControlledFailure{std::move(*invalid)}};
    const auto* captured = to.snapshot(to.context, i);
    if (!captured)
        return std::unexpected{Q2ControlledFailure{std::move(*missing)}};
    const auto t = *captured;
    const auto& b = w[i];
    if (t.worker != b.worker || t.cpu != b.linux_cpu || t.core != b.core ||
        t.pu != b.processing_unit || t.stable_core != b.stable_core ||
        t.stable_pu != b.stable_processing_unit || t.tid.value() == 0 || !t.quiescent)
        return std::unexpected{Q2ControlledFailure{std::move(*crossed)}};
    try {
        auto loop = std::make_unique<LoopState>();
        loop->operations = ho;
        to.attach_protocol(to.context, &loop->protocol);
        return MonitoringQ2ControlledEventSession{
            t, i, to, ho, 0, std::move(*overflow), std::move(*crossed), std::move(loop)};
    } catch (...) {
        return std::unexpected{Q2ControlledFailure{std::move(*invalid)}};
    }
}
MonitoringQ2ControlledEventSession::MonitoringQ2ControlledEventSession(
    MonitoringQ2ControlledEventSession&& o) noexcept
    : target_{o.target_}, target_ops_{o.target_ops_}, helper_ops_{o.helper_ops_},
      helper_{o.helper_}, overflow_error_{std::move(o.overflow_error_)},
      binding_error_{std::move(o.binding_error_)}, pending_error_{std::move(o.pending_error_)},
      generation_{o.generation_}, target_index_{o.target_index_},
      winner_{o.winner_.load()}, phase_{o.phase_}, target_ready_{o.target_ready_},
      helper_ready_{o.helper_ready_}, target_terminal_{o.target_terminal_},
      helper_terminal_{o.helper_terminal_}, active_{std::exchange(o.active_, false)},
      target_retired_{o.target_retired_}, helper_active_{o.helper_active_},
      helper_joined_{o.helper_joined_}, helper_closed_{o.helper_closed_},
      helper_verified_{o.helper_verified_}, helper_absent_{o.helper_absent_},
      loop_{std::move(o.loop_)}, compact_failure_stock_{std::move(o.compact_failure_stock_)},
      compact_failure_stock_index_{o.compact_failure_stock_index_} {
    o.helper_active_ = false;
}
MonitoringQ2ControlledEventSession::~MonitoringQ2ControlledEventSession() noexcept {
    if (active_)
        std::terminate();
}
auto MonitoringQ2ControlledEventSession::begin(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    const auto* now = target_ops_.snapshot(target_ops_.context, target_index_);
    auto live = target_ops_.revalidate(target_ops_.context, target_index_, wait);
    if (!live) {
        pending_error_.emplace(std::move(live.error()));
        return Q2EventStatus::helper_failure;
    }
    if (!helper_active_ || !now || now->worker != target_.worker || now->cpu != target_.cpu ||
        now->core != target_.core || now->pu != target_.pu || now->tid != target_.tid ||
        now->stable_core != target_.stable_core || now->stable_pu != target_.stable_pu ||
        !now->quiescent)
        return Q2EventStatus::crossed;
    if (phase_ != Phase::idle || g == 0 || g <= generation_)
        return Q2EventStatus::stale;
    loop_->command_wait.publish(wait);
    loop_->protocol.generation.store(g, std::memory_order_release);
    // A fresh controlled session may reuse the provider protocol while its generation counter
    // restarts at one. Clear every generation-tagged observation before publishing this request so
    // no equal-valued token from the preceding worker can satisfy a current wait.
    loop_->protocol.target_ready.store(0U, std::memory_order_release);
    loop_->protocol.helper_ready.store(0U, std::memory_order_release);
    loop_->protocol.target_running.store(0U, std::memory_order_release);
    loop_->protocol.helper_running.store(0U, std::memory_order_release);
    loop_->protocol.target_terminal.store(0U, std::memory_order_release);
    loop_->protocol.helper_terminal.store(0U, std::memory_order_release);
    loop_->protocol.helper_ceiling.store(0U, std::memory_order_release);
    loop_->protocol.stop.store(false, std::memory_order_release);
    loop_->protocol.winner.store(Q2Winner::undecided, std::memory_order_release);
    loop_->requested_generation.store(g, std::memory_order_release);
    // A wake without a predicate change cannot release the production bounded atomic wait.
    loop_->command.store(readiness_requested, std::memory_order_release);
    if (publication_hook) publication_hook(loop_.get());
    helper_ops_.wake(helper_ops_.context, &loop_->command);
    auto begun = target_ops_.begin(target_ops_.context, target_index_, g, wait);
    if (!begun) {
        pending_error_.emplace(std::move(begun.error()));
        return Q2EventStatus::helper_failure;
    }
    generation_ = g;
    winner_ = Q2Winner::undecided;
    target_ready_ = helper_ready_ = target_terminal_ = helper_terminal_ = false;
    phase_ = Phase::parking;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::ready(
    std::uint64_t g, bool helper, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::parking)
        return Q2EventStatus::crossed;
    auto& slot = helper ? helper_ready_ : target_ready_;
    if (slot)
        return Q2EventStatus::duplicate;
    auto& word = helper ? loop_->protocol.helper_ready : loop_->protocol.target_ready;
    const auto waited = wait_status(word, g, wait, pending_error_);
    if (waited != Q2EventStatus::ok)
        return waited;
    slot = true;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::release_target(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::parking || !target_ready_ || !helper_ready_)
        return Q2EventStatus::missing;
    auto released = target_ops_.release(target_ops_.context, target_index_, g, wait);
    if (!released) {
        pending_error_.emplace(std::move(released.error()));
        return Q2EventStatus::helper_failure;
    }
    phase_ = Phase::target_released;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::target_running(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::target_released)
        return Q2EventStatus::crossed;
    const auto waited = wait_status(loop_->protocol.target_running, g, wait, pending_error_);
    if (waited != Q2EventStatus::ok)
        return waited;
    phase_ = Phase::target_running;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::release_helper(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::target_running)
        return Q2EventStatus::crossed;
    loop_->command.store(1U, std::memory_order_release);
    helper_ops_.wake(helper_ops_.context, &loop_->command);
    phase_ = Phase::helper_released;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::compete(
    std::uint64_t g, Q2Winner w, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::helper_released || w == Q2Winner::undecided)
        return Q2EventStatus::crossed;
    const auto waited = wait_status(loop_->protocol.helper_running, g, wait, pending_error_);
    if (waited != Q2EventStatus::ok)
        return waited;
    if (loop_->protocol.helper_ceiling.load(std::memory_order_acquire) == g)
        return Q2EventStatus::ceiling;
    if (loop_->protocol.winner.load(std::memory_order_acquire) != w)
        return Q2EventStatus::crossed;
    winner_.store(w, std::memory_order_release);
    return w == Q2Winner::ceiling_first ? Q2EventStatus::ceiling : Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::stop(std::uint64_t g,
                                              MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::helper_released)
        return Q2EventStatus::crossed;
    phase_ = Phase::stopping;
    auto target_stopped = target_ops_.stop(target_ops_.context, target_index_, g, wait);
    if (!target_stopped) {
        pending_error_.emplace(std::move(target_stopped.error()));
        return Q2EventStatus::helper_failure;
    }
    loop_->command.store(2U, std::memory_order_release);
    helper_ops_.wake(helper_ops_.context, &loop_->command);
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::terminal(
    std::uint64_t g, bool helper, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::stopping)
        return Q2EventStatus::crossed;
    auto& slot = helper ? helper_terminal_ : target_terminal_;
    if (slot)
        return Q2EventStatus::duplicate;
    auto observed = helper
                        ? helper_ops_.drain_terminal(helper_ops_.context, helper_, wait)
                        : target_ops_.drain_terminal(target_ops_.context, target_index_, g, wait);
    if (!observed) {
        pending_error_.emplace(std::move(observed.error()));
        return Q2EventStatus::helper_failure;
    }
    if (helper && loop_->protocol.helper_terminal.load(std::memory_order_acquire) != g)
        return Q2EventStatus::missing;
    if (helper && loop_->protocol.helper_ceiling.load(std::memory_order_acquire) == g)
        return Q2EventStatus::ceiling;
    if (!helper && loop_->protocol.target_terminal.load(std::memory_order_acquire) != g)
        return Q2EventStatus::missing;
    slot = true;
    if (target_terminal_ && helper_terminal_)
        phase_ = Phase::terminal;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::finish(
    std::uint64_t g, Q2GenerationResult* out, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::terminal || !out)
        return Q2EventStatus::missing;
    loop_->command_wait.publish({wait.deadline, {}});
    loop_->park_offer.store(g, std::memory_order_release);
    monitoring_q2_atomic_wake(loop_->park_offer);
    const auto parked = wait_status(loop_->parked_generation, g, wait, pending_error_);
    if (parked != Q2EventStatus::ok)
        return parked;
    auto target_parked = target_ops_.repark(target_ops_.context, target_index_, g, wait);
    if (!target_parked) {
        pending_error_.emplace(std::move(target_parked.error()));
        return Q2EventStatus::helper_failure;
    }
    *out = {g, winner_.load(), true, true};
    phase_ = Phase::idle;
    return out->winner == Q2Winner::helper_first ? Q2EventStatus::ok : Q2EventStatus::ceiling;
}
auto MonitoringQ2ControlledEventSession::begin_voluntary(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (phase_ != Phase::idle || g == 0 || g <= generation_)
        return Q2EventStatus::stale;
    auto begun = target_ops_.begin_voluntary(target_ops_.context, target_index_, g, wait);
    if (!begun) {
        pending_error_.emplace(std::move(begun.error()));
        return Q2EventStatus::helper_failure;
    }
    generation_ = g;
    phase_ = Phase::voluntary_parking;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::voluntary_ready(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::voluntary_parking)
        return Q2EventStatus::duplicate;
    {
        const auto waited = wait_status(loop_->protocol.voluntary_ready, g, wait, pending_error_);
        if (waited != Q2EventStatus::ok)
            return waited;
    }
    phase_ = Phase::voluntary_ready;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::release_wait(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::voluntary_ready)
        return Q2EventStatus::crossed;
    auto released = target_ops_.release_to_wait(target_ops_.context, target_index_, g, wait);
    if (!released) {
        pending_error_.emplace(std::move(released.error()));
        return Q2EventStatus::helper_failure;
    }
    if (loop_->protocol.voluntary_parked.load(std::memory_order_acquire) != g)
        return Q2EventStatus::missing;
    phase_ = Phase::voluntary_waiting;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::wake(std::uint64_t g,
                                              MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::voluntary_waiting)
        return Q2EventStatus::crossed;
    auto woken = target_ops_.wake_voluntary(target_ops_.context, target_index_, g, wait);
    if (!woken) {
        pending_error_.emplace(std::move(woken.error()));
        return Q2EventStatus::helper_failure;
    }
    phase_ = Phase::voluntary_woken;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::resumed(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::voluntary_woken)
        return Q2EventStatus::crossed;
    {
        const auto waited = wait_status(loop_->protocol.voluntary_resumed, g, wait, pending_error_);
        if (waited != Q2EventStatus::ok)
            return waited;
    }
    phase_ = Phase::voluntary_resumed;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::finish_voluntary(
    std::uint64_t g, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2EventStatus {
    if (!active_)
        std::terminate();
    if (g != generation_)
        return Q2EventStatus::stale;
    if (phase_ != Phase::voluntary_resumed)
        return Q2EventStatus::missing;
    auto finished = target_ops_.finish_voluntary(target_ops_.context, target_index_, g, wait);
    if (!finished) {
        pending_error_.emplace(std::move(finished.error()));
        return Q2EventStatus::helper_failure;
    }
    if (loop_->protocol.voluntary_terminal.load(std::memory_order_acquire) != g)
        return Q2EventStatus::missing;
    phase_ = Phase::idle;
    return Q2EventStatus::ok;
}
auto MonitoringQ2ControlledEventSession::finish_voluntary_and_prepare_helper(
    MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, Q2ControlledFailure> {
    if (!active_ || helper_active_)
        std::terminate();
    if (phase_ != Phase::idle)
        return std::unexpected{Q2ControlledFailure{binding_error_}};
    const auto* now = target_ops_.snapshot(target_ops_.context, target_index_);
    auto live = target_ops_.revalidate(target_ops_.context, target_index_, wait);
    if (!live)
        return std::unexpected{Q2ControlledFailure{std::move(live.error())}};
    if (!now || now->worker != target_.worker || now->cpu != target_.cpu ||
        now->core != target_.core || now->pu != target_.pu || now->tid != target_.tid ||
        now->stable_core != target_.stable_core || now->stable_pu != target_.stable_pu ||
        !now->quiescent)
        return std::unexpected{Q2ControlledFailure{binding_error_}};
    loop_->command_wait.publish({wait.deadline, {}});
    auto launched = helper_ops_.launch(helper_ops_.context, loop_.get(), helper_entry, wait);
    if (launched.handle) {
        helper_ = *launched.handle;
        helper_active_ = true;
    }
    if (!launched) {
        if (launched.handle)
            pending_error_.emplace(*launched.failure);
        return std::unexpected{Q2ControlledFailure{*launched.failure}};
    }
    helper_ = *launched.handle;
    helper_active_ = true;
    helper_joined_ = helper_closed_ = helper_verified_ = helper_absent_ = false;
    const auto fail = [&](core::Error primary) {
        Q2ControlledFailure f{std::move(primary)};
        loop_->request_stop();
        for (auto op : {helper_ops_.request_stop, helper_ops_.unblock}) {
            auto r = op(helper_ops_.context, helper_);
            if (!r)
                append(f, std::move(r.error()), overflow_error_);
        }
        auto terminal = helper_ops_.drain_terminal(helper_ops_.context, helper_, wait);
        if (!terminal)
            append(f, std::move(terminal.error()), overflow_error_);
        auto joined = helper_ops_.join(helper_ops_.context, helper_, wait);
        helper_joined_ = joined.has_value();
        if (!joined)
            append(f, joined.error());
        if (!helper_closed_) {
            auto r = helper_ops_.close(helper_ops_.context, helper_);
            helper_closed_ = r.has_value();
            if (!r)
                append(f, std::move(r.error()), overflow_error_);
        }
        auto verified = helper_ops_.verify_terminal(helper_ops_.context, helper_, wait);
        helper_verified_ = verified.has_value();
        if (!verified)
            append(f, std::move(verified.error()), overflow_error_);
        auto absent = helper_ops_.registry_absent(helper_ops_.context, helper_);
        helper_absent_ = absent && *absent;
        if (!absent)
            append(f, std::move(absent.error()), overflow_error_);
        else if (!*absent)
            append(f, core::Error{overflow_error_}, overflow_error_);
        helper_active_ = !(helper_joined_ && helper_closed_ && helper_verified_ && helper_absent_);
        return std::expected<void, Q2ControlledFailure>{std::unexpected{std::move(f)}};
    };
    auto entered = helper_ops_.registry_enter(helper_ops_.context, helper_);
    if (!entered)
        return fail(std::move(entered.error()));
    auto bound = helper_ops_.bind_cpu(helper_ops_.context, helper_, target_.cpu);
    if (!bound)
        return fail(std::move(bound.error()));
    auto verified = helper_ops_.verify_other_affinity(helper_ops_.context, helper_, target_.cpu);
    if (!verified)
        return fail(std::move(verified.error()));
    auto released = helper_ops_.release_start(helper_ops_.context, helper_);
    if (!released) {
        auto result = fail(binding_error_);
        result.error().compact_cleanup.append(released.error());
        return result;
    }
    return {};
}
auto MonitoringQ2ControlledEventSession::abort(
    std::uint64_t g, core::Error cause, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, Q2ControlledFailure> {
    if (!active_)
        std::terminate();
    if (g != generation_)
        std::terminate();
    Q2ControlledFailure failure{std::move(cause)};
    const auto record = [&](Q2LifecycleResult result) noexcept {
        if (!result)
            append(failure, std::move(result.error()), overflow_error_);
    };
    const bool voluntary = phase_ == Phase::voluntary_parking || phase_ == Phase::voluntary_ready ||
                           phase_ == Phase::voluntary_waiting || phase_ == Phase::voluntary_woken ||
                           phase_ == Phase::voluntary_resumed;
    if (helper_active_) {
        loop_->request_stop();
        record(helper_ops_.request_stop(helper_ops_.context, helper_));
    }
    record(target_ops_.stop(target_ops_.context, target_index_, g, wait));
    if (voluntary) {
        if (phase_ == Phase::voluntary_parking || phase_ == Phase::voluntary_ready)
            record(target_ops_.release_to_wait(target_ops_.context, target_index_, g, wait));
        record(target_ops_.wake_voluntary(target_ops_.context, target_index_, g, wait));
    } else {
        if (phase_ == Phase::parking)
            record(target_ops_.release(target_ops_.context, target_index_, g, wait));
    }
    if (helper_active_)
        record(helper_ops_.unblock(helper_ops_.context, helper_));
    record(target_ops_.drain_terminal(target_ops_.context, target_index_, g, wait));
    if (helper_active_)
        record(helper_ops_.drain_terminal(helper_ops_.context, helper_, wait));
    record(target_ops_.repark(target_ops_.context, target_index_, g, wait));
    if (!target_retired_) {
        auto target = target_ops_.quiesce(target_ops_.context, target_index_, wait);
        target_retired_ = target.has_value();
        record(std::move(target));
    }
    if (helper_active_) {
        auto joined = helper_ops_.join(helper_ops_.context, helper_, wait);
        helper_joined_ = joined.has_value();
        if (!joined)
            append(failure, joined.error());
        if (!helper_closed_) {
            auto closed = helper_ops_.close(helper_ops_.context, helper_);
            helper_closed_ = closed.has_value();
            record(std::move(closed));
        }
        auto verified = helper_ops_.verify_terminal(helper_ops_.context, helper_, wait);
        helper_verified_ = verified.has_value();
        record(std::move(verified));
        auto absent = helper_ops_.registry_absent(helper_ops_.context, helper_);
        helper_absent_ = absent && *absent;
        if (!absent)
            record(std::unexpected{std::move(absent.error())});
        else if (!*absent)
            record(std::unexpected{overflow_error_});
    }
    helper_active_ =
        helper_active_ && !(helper_joined_ && helper_closed_ && helper_verified_ && helper_absent_);
    active_ = !target_retired_ || helper_active_;
    phase_ = Phase::retired;
    return std::unexpected{std::move(failure)};
}
auto MonitoringQ2ControlledEventSession::retire(
    MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, Q2ControlledFailure> {
    if (!active_)
        std::terminate();
    std::optional<Q2ControlledFailure> failure;
    if (pending_error_) {
        failure.emplace(Q2ControlledFailure{std::move(*pending_error_)});
        pending_error_.reset();
    }
    const auto record = [&](Q2LifecycleResult r) {
        if (r)
            return;
        if (!failure)
            failure.emplace(Q2ControlledFailure{std::move(r.error())});
        else
            append(*failure, std::move(r.error()), overflow_error_);
    };
    const auto* now = target_ops_.snapshot(target_ops_.context, target_index_);
    auto live = target_ops_.revalidate(target_ops_.context, target_index_, wait);
    bool target_revalidated = live.has_value();
    if (!live)
        record(std::unexpected{std::move(live.error())});
    else if (!now || now->worker != target_.worker || now->cpu != target_.cpu ||
             now->core != target_.core || now->pu != target_.pu || now->tid != target_.tid ||
             now->stable_core != target_.stable_core || now->stable_pu != target_.stable_pu ||
             !now->quiescent) {
        target_revalidated = false;
        record(std::unexpected{binding_error_});
    }
    if (!target_retired_ && target_revalidated) {
        auto target = target_ops_.quiesce(target_ops_.context, target_index_, wait);
        target_retired_ = target.has_value();
        record(std::move(target));
    }
    if (helper_active_) {
        loop_->request_stop();
        for (auto op : {helper_ops_.request_stop, helper_ops_.unblock})
            record(op(helper_ops_.context, helper_));
        record(helper_ops_.drain_terminal(helper_ops_.context, helper_, wait));
        if (!helper_joined_) {
            auto joined = helper_ops_.join(helper_ops_.context, helper_, wait);
            helper_joined_ = joined.has_value();
            if (!joined) {
                if (!failure) {
                    if (compact_failure_stock_index_ >= compact_failure_stock_.size())
                        std::terminate();
                    failure.emplace(
                        std::move(compact_failure_stock_[compact_failure_stock_index_++]));
                }
                append(*failure, joined.error());
            }
        }
        if (!helper_closed_) {
            auto closed = helper_ops_.close(helper_ops_.context, helper_);
            helper_closed_ = closed.has_value();
            record(std::move(closed));
        }
        if (!helper_verified_) {
            auto verified = helper_ops_.verify_terminal(helper_ops_.context, helper_, wait);
            helper_verified_ = verified.has_value();
            record(std::move(verified));
        }
    }
    auto absent = helper_active_ ? helper_ops_.registry_absent(helper_ops_.context, helper_)
                                 : std::expected<bool, core::Error>{true};
    if (!absent)
        record(std::unexpected{std::move(absent.error())});
    else if (!*absent)
        record(std::unexpected{overflow_error_});
    else
        helper_absent_ = true;
    phase_ = Phase::retired;
    helper_active_ =
        helper_active_ && !(helper_joined_ && helper_closed_ && helper_verified_ && helper_absent_);
    active_ = !target_retired_ || helper_active_;
    if (failure)
        return std::unexpected{std::move(*failure)};
    return {};
}
} // namespace epycbench::monitoring::qualification::detail
