#include "monitoring/host_q2_native_thread_owner.hpp"
#include <cerrno>
#include <limits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
std::array<std::atomic<std::uint64_t>, 8> target_registry{};
std::atomic<std::uint64_t> helper_registry{};
std::atomic<std::uint64_t> next_token{1};

constexpr auto diagnostic(MonitoringQ2CompactOperation operation, int native = 0) noexcept
    -> MonitoringQ2CompactDiagnostic {
    return {operation, native};
}
auto wait_error(MonitoringQ2CompactOperation operation, int native = 0) -> core::Error {
    return monitoring_q2_materialize(diagnostic(operation, native));
}
auto default_system() noexcept -> const MonitoringQ2NativeThreadSystem& {
    static const MonitoringQ2NativeThreadSystem value{
        nullptr,
        [](void*, clockid_t c, timespec* t) noexcept { return ::clock_gettime(c, t); },
        [](void*, pthread_mutex_t* m, const pthread_mutexattr_t* a) noexcept {
            return pthread_mutex_init(m, a);
        },
        [](void*, pthread_mutex_t* m) noexcept { return pthread_mutex_destroy(m); },
        [](void*, pthread_mutex_t* m) noexcept { return pthread_mutex_lock(m); },
        [](void*, pthread_mutex_t* m) noexcept { return pthread_mutex_unlock(m); },
        [](void*, pthread_condattr_t* a) noexcept { return pthread_condattr_init(a); },
        [](void*, pthread_condattr_t* a, clockid_t c) noexcept {
            return pthread_condattr_setclock(a, c);
        },
        [](void*, pthread_condattr_t* a) noexcept { return pthread_condattr_destroy(a); },
        [](void*, pthread_cond_t* c, const pthread_condattr_t* a) noexcept {
            return pthread_cond_init(c, a);
        },
        [](void*, pthread_cond_t* c) noexcept { return pthread_cond_destroy(c); },
        [](void*, pthread_cond_t* c) noexcept { return pthread_cond_broadcast(c); },
        [](void*, pthread_cond_t* c, pthread_mutex_t* m, const timespec* t) noexcept {
            return pthread_cond_timedwait(c, m, t);
        },
        [](void*, pthread_t* t, const pthread_attr_t* a, void* (*f)(void*), void* p) noexcept {
            return pthread_create(t, a, f, p);
        },
        [](void*, pthread_t t, void** p, const timespec* d) noexcept {
            return pthread_timedjoin_np(t, p, d);
        },
        [](void*, pthread_t t, void** p, clockid_t clock, const timespec* d) noexcept {
            return pthread_clockjoin_np(t, p, clock, d);
        }};
    return value;
}
auto complete(const MonitoringQ2NativeThreadSystem& s) noexcept -> bool {
    return s.clock_gettime && s.mutex_init && s.mutex_destroy && s.mutex_lock && s.mutex_unlock &&
           s.condattr_init && s.condattr_setclock && s.condattr_destroy && s.cond_init &&
           s.cond_destroy && s.cond_broadcast && s.cond_timedwait && s.thread_create &&
           s.thread_timedjoin && s.thread_clockjoin;
}
auto valid_cancellation(MonitoringQ2WaitCancellation value) noexcept -> bool {
    const bool paired =
        static_cast<bool>(value.register_wake) == static_cast<bool>(value.unregister_wake);
    return paired && (!value.requested || value.register_wake);
}
auto reserve(MonitoringQ2NativeThreadSlot slot, std::uint64_t token) noexcept -> bool {
    if (slot.role == MonitoringQ2NativeThreadRole::helper) {
        std::uint64_t empty{};
        return helper_registry.compare_exchange_strong(empty, token);
    }
    if (slot.target_index >= MonitoringQ2NativeThreadOwner::maximum_targets)
        return false;
    auto& word = target_registry[slot.target_index / 64];
    const auto mask = std::uint64_t{1} << (slot.target_index % 64);
    auto old = word.load(std::memory_order_relaxed);
    while (!(old & mask)) {
        if (word.compare_exchange_weak(old, old | mask, std::memory_order_acq_rel))
            return true;
    }
    return false;
}
void release(MonitoringQ2NativeThreadSlot slot, std::uint64_t token) noexcept {
    if (slot.role == MonitoringQ2NativeThreadRole::helper) {
        auto held = token;
        static_cast<void>(helper_registry.compare_exchange_strong(held, 0));
        return;
    }
    target_registry[slot.target_index / 64].fetch_and(
        ~(std::uint64_t{1} << (slot.target_index % 64)), std::memory_order_acq_rel);
}
auto add_ns(timespec now, std::uint64_t delta) noexcept -> timespec {
    constexpr auto billion = std::uint64_t{1000000000};
    const auto seconds = delta / billion;
    const auto nanos = delta % billion;
    const auto sum = static_cast<std::uint64_t>(now.tv_nsec) + nanos;
    const auto max = static_cast<std::uint64_t>(std::numeric_limits<time_t>::max());
    auto base = static_cast<std::uint64_t>(now.tv_sec);
    if (seconds > max - base)
        return {std::numeric_limits<time_t>::max(), 999999999};
    base += seconds;
    if (sum >= billion) {
        if (base == max)
            return {std::numeric_limits<time_t>::max(), 999999999};
        ++base;
    }
    return {static_cast<time_t>(base), static_cast<long>(sum % billion)};
}
struct ClockSnapshot {
    timespec absolute{};
    std::optional<MonitoringQ2CompactDiagnostic> failure;
};
auto convert(const MonitoringQ2NativeThreadSystem& s, clockid_t target,
             MonitoringQ2RawDeadline d) noexcept -> ClockSnapshot {
    timespec raw{}, now{};
    if (s.clock_gettime(s.context, target, &now) != 0)
        return {{}, diagnostic(MonitoringQ2CompactOperation::clock_target, errno)};
    if (s.clock_gettime(s.context, CLOCK_MONOTONIC_RAW, &raw) != 0)
        return {{}, diagnostic(MonitoringQ2CompactOperation::clock_raw, errno)};
    constexpr auto billion = std::uint64_t{1000000000};
    const auto normalized = [](const timespec& value) noexcept {
        return value.tv_sec >= 0 && value.tv_nsec >= 0 &&
               static_cast<std::uint64_t>(value.tv_nsec) < billion;
    };
    if (!normalized(raw) || !normalized(now))
        return {{}, diagnostic(MonitoringQ2CompactOperation::clock_range, ERANGE)};
    const auto raw_seconds = static_cast<std::uint64_t>(raw.tv_sec);
    const auto raw_nanos = static_cast<std::uint64_t>(raw.tv_nsec);
    if (raw_seconds > (std::numeric_limits<std::uint64_t>::max() - raw_nanos) / billion)
        return {{}, diagnostic(MonitoringQ2CompactOperation::clock_range, ERANGE)};
    const auto raw_ns = raw_seconds * billion + raw_nanos;
    return {add_ns(now, d.nanoseconds > raw_ns ? d.nanoseconds - raw_ns : 0), std::nullopt};
}
} // namespace

struct MonitoringQ2NativeThreadOwner::State final {
    const MonitoringQ2NativeThreadSystem* system{};
    MonitoringQ2NativeThreadSlot role{};
    std::uint64_t identity{};
    pthread_t thread{};
    pthread_mutex_t mutex{};
    pthread_cond_t condition{};
    pthread_condattr_t condition_attribute{};
    bool mutex_ready{}, condition_ready{}, condition_attribute_ready{}, thread_created{};
    void* context{};
    Entry entry{};
    Prepare prepare{};
    unsigned gate{};
    bool parked{}, terminal{}, joined{}, quarantine{};
    timespec gate_deadline{};
    std::optional<core::Error> preparation_failure;
    std::atomic<bool> stop{};
    MonitoringQ2CompactDiagnostic state_error, error_cancel, error_timeout;
    MonitoringQ2CompactLedger<2> cleanup_diagnostics;
    State(const MonitoringQ2NativeThreadSystem& sys, MonitoringQ2NativeThreadSlot r,
          std::uint64_t token, void* c, Entry e, Prepare p, timespec deadline)
        : system{&sys}, role{r}, identity{token}, context{c}, entry{e}, prepare{p},
          gate_deadline{deadline}, state_error{diagnostic(MonitoringQ2CompactOperation::state)},
          error_cancel{diagnostic(MonitoringQ2CompactOperation::cancelled, ECANCELED)},
          error_timeout{diagnostic(MonitoringQ2CompactOperation::timeout, ETIMEDOUT)} {}
    ~State() {
        if (condition_ready || condition_attribute_ready || mutex_ready)
            std::terminate();
    }
    static void* run(void* raw) noexcept {
        auto& s = *static_cast<State*>(raw);
        auto& o = *s.system;
        if (s.prepare) {
            auto prepared = s.prepare(s.context);
            if (!prepared)
                s.preparation_failure.emplace(std::move(prepared.error()));
        }
        if (o.mutex_lock(o.context, &s.mutex) != 0)
            return nullptr;
        s.parked = true;
        static_cast<void>(o.cond_broadcast(o.context, &s.condition));
        while (s.gate == 0 && !s.stop.load(std::memory_order_acquire)) {
            const auto rc = o.cond_timedwait(o.context, &s.condition, &s.mutex, &s.gate_deadline);
            if (rc == ETIMEDOUT || (rc && rc != EINTR))
                break;
        }
        const bool invoke = s.gate == 1 && !s.stop.load(std::memory_order_acquire);
        static_cast<void>(o.mutex_unlock(o.context, &s.mutex));
        if (invoke)
            s.entry(s.context, StopToken{&s.stop});
        if (o.mutex_lock(o.context, &s.mutex) == 0) {
            s.terminal = true;
            static_cast<void>(o.cond_broadcast(o.context, &s.condition));
            static_cast<void>(o.mutex_unlock(o.context, &s.mutex));
        }
        return nullptr;
    }
    static void cancellation_wake(void* raw) noexcept {
        auto& s = *static_cast<State*>(raw);
        auto& o = *s.system;
        static_cast<void>(o.cond_broadcast(o.context, &s.condition));
    }
};

MonitoringQ2NativeThreadOwner::MonitoringQ2NativeThreadOwner(std::unique_ptr<State> s) noexcept
    : state_{std::move(s)} {}
MonitoringQ2NativeThreadOwner::MonitoringQ2NativeThreadOwner(
    MonitoringQ2NativeThreadOwner&&) noexcept = default;
auto MonitoringQ2NativeThreadOwner::operator=(MonitoringQ2NativeThreadOwner&& other) noexcept
    -> MonitoringQ2NativeThreadOwner& {
    if (this != &other && state_ && !state_->joined)
        std::terminate();
    state_ = std::move(other.state_);
    return *this;
}
MonitoringQ2NativeThreadOwner::~MonitoringQ2NativeThreadOwner() noexcept {
    if (state_ && !state_->joined)
        std::terminate();
}

auto MonitoringQ2NativeThreadOwner::create_parked(
    void* c, Entry e, MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> MonitoringQ2NativeThreadCreateOutcome {
    return create_parked({MonitoringQ2NativeThreadRole::helper, 0}, c, e, wait, nullptr, nullptr);
}
auto MonitoringQ2NativeThreadOwner::create_parked(MonitoringQ2NativeThreadSlot role, void* c,
                                                  Entry e,
                                                  MonitoringQ2DeadlineCancellationContext wait,
                                                  const MonitoringQ2NativeThreadSystem* injected,
                                                  Prepare prepare,
                                                  const pthread_attr_t* attributes,
                                                  std::uint64_t cancellation_poll_ns) noexcept
    -> MonitoringQ2NativeThreadCreateOutcome {
    auto fail = [](MonitoringQ2CompactDiagnostic cause) {
        return MonitoringQ2NativeThreadCreateOutcome{
            std::nullopt, MonitoringQ2NativeThreadCreateFailure{std::move(cause)}};
    };
    const auto& sys = injected ? *injected : default_system();
    const bool polled = cancellation_poll_ns > 0U && cancellation_poll_ns <= UINT64_C(10000000) &&
        wait.cancellation.requested && !wait.cancellation.register_wake && !wait.cancellation.unregister_wake;
    if (!e || !complete(sys) ||
        (cancellation_poll_ns ? !polled : !valid_cancellation(wait.cancellation)))
        return fail(diagnostic(MonitoringQ2CompactOperation::operations));
    const auto token = next_token.fetch_add(1, std::memory_order_relaxed);
    if (token == 0 || !reserve(role, token))
        return fail(diagnostic(MonitoringQ2CompactOperation::occupied));
    const auto gate_snapshot = convert(sys, CLOCK_MONOTONIC, wait.deadline);
    if (gate_snapshot.failure) {
        release(role, token);
        return fail(*gate_snapshot.failure);
    }
    std::unique_ptr<State> state;
    try {
        state = std::make_unique<State>(sys, role, token, c, e, prepare, gate_snapshot.absolute);
    } catch (...) {
        release(role, token);
        return fail(diagnostic(MonitoringQ2CompactOperation::construction));
    }
    auto prethread = [&](MonitoringQ2CompactOperation operation, int native) {
        MonitoringQ2NativeThreadCreateFailure failure{diagnostic(operation, native)};
        if (state->condition_ready) {
            const auto cleanup = sys.cond_destroy(sys.context, &state->condition);
            if (cleanup)
                failure.cleanup.append(diagnostic(MonitoringQ2CompactOperation::cond_destroy, cleanup));
            else
                state->condition_ready = false;
        }
        if (state->condition_attribute_ready) {
            const auto cleanup = sys.condattr_destroy(sys.context, &state->condition_attribute);
            if (cleanup)
                failure.cleanup.append(diagnostic(MonitoringQ2CompactOperation::condattr_destroy, cleanup));
            else
                state->condition_attribute_ready = false;
        }
        if (state->mutex_ready) {
            const auto cleanup = sys.mutex_destroy(sys.context, &state->mutex);
            if (cleanup)
                failure.cleanup.append(diagnostic(MonitoringQ2CompactOperation::mutex_destroy, cleanup));
            else
                state->mutex_ready = false;
        }
        if (failure.cleanup.size() != 0U) {
            state->quarantine = true;
            return MonitoringQ2NativeThreadCreateOutcome{
                MonitoringQ2NativeThreadOwner{std::move(state)}, std::move(failure)};
        }
        release(role, token);
        state.reset();
        return MonitoringQ2NativeThreadCreateOutcome{std::nullopt, std::move(failure)};
    };
    int rc = sys.mutex_init(sys.context, &state->mutex, nullptr);
    if (rc)
        return prethread(MonitoringQ2CompactOperation::mutex_init, rc);
    state->mutex_ready = true;
    rc = sys.condattr_init(sys.context, &state->condition_attribute);
    if (rc)
        return prethread(MonitoringQ2CompactOperation::condattr_init, rc);
    state->condition_attribute_ready = true;
    rc = sys.condattr_setclock(sys.context, &state->condition_attribute, CLOCK_MONOTONIC);
    if (rc)
        return prethread(MonitoringQ2CompactOperation::condattr_setclock, rc);
    rc = sys.cond_init(sys.context, &state->condition, &state->condition_attribute);
    if (rc)
        return prethread(MonitoringQ2CompactOperation::cond_init, rc);
    state->condition_ready = true;
    const auto attr_destroy = sys.condattr_destroy(sys.context, &state->condition_attribute);
    if (!attr_destroy)
        state->condition_attribute_ready = false;
    if (attr_destroy)
        return prethread(MonitoringQ2CompactOperation::condattr_destroy, attr_destroy);
    rc = sys.thread_create(sys.context, &state->thread, attributes, State::run, state.get());
    if (rc)
        return prethread(MonitoringQ2CompactOperation::create, rc);
    state->thread_created = true;
    MonitoringQ2NativeThreadCreateFailure failure{diagnostic(MonitoringQ2CompactOperation::wait)};
    bool failed{};
    const auto snapshot = convert(sys, CLOCK_MONOTONIC, wait.deadline);
    if (snapshot.failure) {
        failure.primary = std::move(*snapshot.failure);
        failed = true;
    }
    if (!failed && (rc = sys.mutex_lock(sys.context, &state->mutex)) != 0) {
        failure.primary = diagnostic(MonitoringQ2CompactOperation::mutex_lock, rc);
        failed = true;
    }
    bool locked = !failed;
    const auto cancellation = wait.cancellation;
    if (locked && cancellation.register_wake)
        cancellation.register_wake(cancellation.context, state.get(), State::cancellation_wake);
    if (locked && cancellation.requested && cancellation.requested(cancellation.context)) {
        failure.primary = state->error_cancel;
        failed = true;
    }
    while (locked && !state->parked) {
        if (cancellation.requested && cancellation.requested(cancellation.context)) {
            failure.primary = state->error_cancel;
            failed = true;
            break;
        }
        auto slice = snapshot.absolute;
        bool intermediate{};
        if (polled) {
            timespec now{};
            if (sys.clock_gettime(sys.context, CLOCK_MONOTONIC, &now) != 0) {
                failure.primary = diagnostic(MonitoringQ2CompactOperation::clock_target, errno);
                failed = true; break;
            }
            if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
                failure.primary = diagnostic(MonitoringQ2CompactOperation::clock_range, ERANGE);
                failed = true; break;
            }
            const auto candidate = add_ns(now, cancellation_poll_ns);
            if (candidate.tv_sec < slice.tv_sec ||
                (candidate.tv_sec == slice.tv_sec && candidate.tv_nsec < slice.tv_nsec)) {
                slice = candidate; intermediate = true;
            }
        }
        rc = sys.cond_timedwait(sys.context, &state->condition, &state->mutex, &slice);
        if (rc == ETIMEDOUT) {
            if (intermediate) continue;
            failure.primary = state->error_timeout;
            failed = true;
            break;
        }
        if (rc && rc != EINTR) {
            failure.primary = diagnostic(MonitoringQ2CompactOperation::wait, rc);
            failed = true;
            break;
        }
    }
    if (locked && cancellation.requested && cancellation.requested(cancellation.context)) {
        failure.primary = state->error_cancel;
        failed = true;
    }
    if (locked) {
        rc = sys.mutex_unlock(sys.context, &state->mutex);
        if (rc && !failed) {
            failure.primary = diagnostic(MonitoringQ2CompactOperation::mutex_unlock, rc);
            failed = true;
        }
    }
    if (cancellation.unregister_wake &&
        cancellation.unregister_wake(cancellation.context, state.get()) !=
            MonitoringQ2CancellationUnregister::quiescent) {
        std::terminate();
    }
    if (!failed && state->preparation_failure) {
        failure.authored_primary.emplace(std::move(*state->preparation_failure));
        failure.primary = diagnostic(MonitoringQ2CompactOperation::state);
        failed = true;
    }
    if (!failed)
        return MonitoringQ2NativeThreadCreateOutcome{
            MonitoringQ2NativeThreadOwner{std::move(state)}, std::nullopt};
    state->quarantine = true;
    state->stop.store(true, std::memory_order_release);
    static_cast<void>(sys.cond_broadcast(sys.context, &state->condition));
    return MonitoringQ2NativeThreadCreateOutcome{MonitoringQ2NativeThreadOwner{std::move(state)},
                                                 std::move(failure)};
}
auto MonitoringQ2NativeThreadOwner::release_start() noexcept
    -> std::expected<void, MonitoringQ2CompactDiagnostic> {
    if (!state_ || state_->joined)
        std::terminate();
    auto& o = *state_->system;
    auto rc = o.mutex_lock(o.context, &state_->mutex);
    if (rc)
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::mutex_lock, rc)};
    if (state_->quarantine || !state_->parked || state_->terminal || state_->gate) {
        static_cast<void>(o.mutex_unlock(o.context, &state_->mutex));
        return std::unexpected{state_->state_error};
    }
    state_->gate = 1;
    rc = o.cond_broadcast(o.context, &state_->condition);
    const auto unlocked = o.mutex_unlock(o.context, &state_->mutex);
    if (rc)
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::wake, rc)};
    if (unlocked)
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::mutex_unlock, unlocked)};
    return {};
}
void MonitoringQ2NativeThreadOwner::request_stop() noexcept {
    if (!state_ || state_->joined)
        return;
    state_->stop.store(true, std::memory_order_release);
    auto& o = *state_->system;
    if (o.mutex_lock(o.context, &state_->mutex) == 0) {
        if (state_->gate == 0)
            state_->gate = 2;
        static_cast<void>(o.cond_broadcast(o.context, &state_->condition));
        static_cast<void>(o.mutex_unlock(o.context, &state_->mutex));
    }
}
auto MonitoringQ2NativeThreadOwner::timed_join(
    MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, MonitoringQ2CompactDiagnostic> {
    if (!state_ || state_->joined)
        std::terminate();
    if (!valid_cancellation(wait.cancellation))
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::cancellation_contract)};
    auto& o = *state_->system;
    if (!state_->thread_created) {
        std::optional<MonitoringQ2CompactDiagnostic> failure;
        if (state_->condition_ready) {
            const auto rc = o.cond_destroy(o.context, &state_->condition);
            if (rc)
                failure.emplace(diagnostic(MonitoringQ2CompactOperation::cond_destroy, rc));
            else
                state_->condition_ready = false;
        }
        if (state_->condition_attribute_ready) {
            const auto rc = o.condattr_destroy(o.context, &state_->condition_attribute);
            if (rc) {
                const auto item=diagnostic(MonitoringQ2CompactOperation::condattr_destroy, rc);
                if (!failure) failure.emplace(item); else state_->cleanup_diagnostics.append(item);
            }
            else if (!rc)
                state_->condition_attribute_ready = false;
        }
        if (state_->mutex_ready) {
            const auto rc = o.mutex_destroy(o.context, &state_->mutex);
            if (rc) {
                const auto item=diagnostic(MonitoringQ2CompactOperation::mutex_destroy, rc);
                if (!failure) failure.emplace(item); else state_->cleanup_diagnostics.append(item);
            }
            else if (!rc)
                state_->mutex_ready = false;
        }
        if (failure) {
            state_->quarantine = true;
            return std::unexpected{std::move(*failure)};
        }
        state_->joined = true;
        state_->quarantine = false;
        release(state_->role, state_->identity);
        return {};
    }
    const auto wait_snapshot = convert(o, CLOCK_MONOTONIC, wait.deadline);
    if (wait_snapshot.failure) {
        state_->quarantine = true;
        return std::unexpected{std::move(*wait_snapshot.failure)};
    }
    bool cancelled{};
    auto rc = o.mutex_lock(o.context, &state_->mutex);
    if (rc) {
        state_->quarantine = true;
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::mutex_lock, rc)};
    }
    const auto cancellation = wait.cancellation;
    if (cancellation.register_wake)
        cancellation.register_wake(cancellation.context, state_.get(), State::cancellation_wake);
    if (cancellation.requested && cancellation.requested(cancellation.context)) {
        cancelled = true;
        state_->stop.store(true, std::memory_order_release);
        if (state_->gate == 0)
            state_->gate = 2;
        static_cast<void>(o.cond_broadcast(o.context, &state_->condition));
    }
    std::optional<MonitoringQ2CompactDiagnostic> wait_failure;
    while (!state_->terminal) {
        if (cancellation.requested && cancellation.requested(cancellation.context)) {
            cancelled = true;
            state_->stop.store(true, std::memory_order_release);
            if (state_->gate == 0)
                state_->gate = 2;
            static_cast<void>(o.cond_broadcast(o.context, &state_->condition));
        }
        rc = o.cond_timedwait(o.context, &state_->condition, &state_->mutex,
                              &wait_snapshot.absolute);
        if (rc == ETIMEDOUT)
            break;
        if (rc && rc != EINTR) {
            wait_failure.emplace(diagnostic(MonitoringQ2CompactOperation::wait, rc));
            break;
        }
    }
    const bool terminal = state_->terminal;
    rc = o.mutex_unlock(o.context, &state_->mutex);
    const auto unlock_failure = rc ? std::optional{diagnostic(MonitoringQ2CompactOperation::mutex_unlock, rc)} : std::nullopt;
    if (cancellation.unregister_wake &&
        cancellation.unregister_wake(cancellation.context, state_.get()) !=
            MonitoringQ2CancellationUnregister::quiescent) {
        state_->quarantine = true;
        std::terminate();
    }
    if (unlock_failure) {
        state_->quarantine = true;
        return std::unexpected{*unlock_failure};
    }
    if (wait_failure) {
        state_->quarantine = true;
        return std::unexpected{std::move(*wait_failure)};
    }
    if (!terminal) {
        state_->quarantine = true;
        return std::unexpected{cancelled ? state_->error_cancel : state_->error_timeout};
    }
    const auto join_snapshot = convert(o, CLOCK_MONOTONIC, wait.deadline);
    if (join_snapshot.failure) {
        state_->quarantine = true;
        return std::unexpected{std::move(*join_snapshot.failure)};
    }
    for (;;) {
        rc = o.thread_clockjoin(o.context, state_->thread, nullptr, CLOCK_MONOTONIC,
                                &join_snapshot.absolute);
        if (rc == EINTR)
            continue;
        if (rc == 0) {
            state_->thread_created = false;
            std::optional<MonitoringQ2CompactDiagnostic> cleanup_failure;
            if (state_->condition_ready) {
                const auto cleanup = o.cond_destroy(o.context, &state_->condition);
                if (cleanup) {
                    cleanup_failure.emplace(diagnostic(MonitoringQ2CompactOperation::cond_destroy, cleanup));
                } else state_->condition_ready = false;
            }
            if (state_->mutex_ready) {
                const auto cleanup = o.mutex_destroy(o.context, &state_->mutex);
                if (cleanup) {
                    const auto item=diagnostic(MonitoringQ2CompactOperation::mutex_destroy, cleanup);
                    if (cleanup_failure) state_->cleanup_diagnostics.append(item);
                    else cleanup_failure.emplace(item);
                } else state_->mutex_ready = false;
            }
            if (cleanup_failure) { state_->quarantine=true; return std::unexpected{*cleanup_failure}; }
            state_->joined = true;
            state_->quarantine = false;
            release(state_->role, state_->identity);
            if (cancelled)
                return std::unexpected{state_->error_cancel};
            return {};
        }
        state_->quarantine = true;
        if (rc == ETIMEDOUT)
            return std::unexpected{cancelled ? state_->error_cancel : state_->error_timeout};
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::join, rc)};
    }
}
auto MonitoringQ2NativeThreadOwner::take_cleanup_diagnostics() noexcept
    -> MonitoringQ2CompactLedger<2> {
    if (!state_) std::terminate();
    auto result=state_->cleanup_diagnostics;
    state_->cleanup_diagnostics={};
    return result;
}
auto MonitoringQ2NativeThreadOwner::native_handle() const noexcept -> pthread_t {
    if (!state_)
        std::terminate();
    return state_->thread;
}
auto MonitoringQ2NativeThreadOwner::active() const noexcept -> bool {
    return state_ && !state_->joined;
}
auto MonitoringQ2NativeThreadOwner::quarantined() const noexcept -> bool {
    return state_ && state_->quarantine && !state_->joined;
}
auto MonitoringQ2NativeThreadOwner::slot() const noexcept -> MonitoringQ2NativeThreadSlot {
    if (!state_)
        std::terminate();
    return state_->role;
}
auto MonitoringQ2NativeThreadOwner::token() const noexcept -> std::uint64_t {
    if (!state_)
        std::terminate();
    return state_->identity;
}
namespace {
struct FutexCancellationCapsule {
    void* address;
};
void futex_wake_capsule(void* raw) noexcept {
    auto& capsule = *static_cast<FutexCancellationCapsule*>(raw);
    static_cast<void>(::syscall(SYS_futex, static_cast<std::uint32_t*>(capsule.address),
                                FUTEX_WAKE_BITSET_PRIVATE, std::numeric_limits<int>::max(), nullptr,
                                nullptr, FUTEX_BITSET_MATCH_ANY));
}
} // namespace
auto monitoring_q2_bounded_atomic_wait_compact(const std::atomic<std::uint32_t>& word,
                                       std::uint32_t expected,
                                       MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, MonitoringQ2CompactDiagnostic> {
    if (!valid_cancellation(wait.cancellation))
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::cancellation_contract)};
    const auto snapshot = convert(default_system(), CLOCK_MONOTONIC, wait.deadline);
    if (snapshot.failure)
        return std::unexpected{*snapshot.failure};
    auto* mutable_word = const_cast<std::atomic<std::uint32_t>*>(&word);
    FutexCancellationCapsule capsule{mutable_word};
    const auto cancellation = wait.cancellation;
    if (cancellation.register_wake)
        cancellation.register_wake(cancellation.context, &capsule, futex_wake_capsule);
    const auto unregister = [&]() -> bool {
        return !cancellation.unregister_wake ||
               cancellation.unregister_wake(cancellation.context, &capsule) ==
                   MonitoringQ2CancellationUnregister::quiescent;
    };
    if (cancellation.requested && cancellation.requested(cancellation.context)) {
        if (!unregister())
            std::terminate();
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::cancelled, ECANCELED)};
    }
    while (word.load(std::memory_order_acquire) == expected) {
        if (cancellation.requested && cancellation.requested(cancellation.context)) {
            if (!unregister())
                std::terminate();
            return std::unexpected{diagnostic(MonitoringQ2CompactOperation::cancelled, ECANCELED)};
        }
        const auto rc = ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(mutable_word),
                                  FUTEX_WAIT_BITSET_PRIVATE, expected, &snapshot.absolute, nullptr,
                                  FUTEX_BITSET_MATCH_ANY);
        if (rc == 0 || errno == EAGAIN || errno == EINTR)
            continue;
        if (errno == ETIMEDOUT) {
            const bool cancelled =
                cancellation.requested && cancellation.requested(cancellation.context);
            if (!unregister())
                std::terminate();
            return std::unexpected{cancelled ? diagnostic(MonitoringQ2CompactOperation::cancelled, ECANCELED)
                                             : diagnostic(MonitoringQ2CompactOperation::timeout, ETIMEDOUT)};
        }
        const auto native = errno;
        if (!unregister())
            std::terminate();
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::wait, native)};
    }
    const bool cancelled = cancellation.requested && cancellation.requested(cancellation.context);
    if (!unregister())
        std::terminate();
    if (cancelled)
        return std::unexpected{diagnostic(MonitoringQ2CompactOperation::cancelled, ECANCELED)};
    return {};
}
auto monitoring_q2_bounded_atomic_wait(const std::atomic<std::uint32_t>& word,
                                       std::uint32_t expected,
                                       MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, core::Error> {
    auto result = monitoring_q2_bounded_atomic_wait_compact(word, expected, wait);
    if (!result) return std::unexpected{monitoring_q2_materialize(result.error())};
    return {};
}
auto monitoring_q2_bounded_atomic_wait(const std::atomic<std::uint64_t>& word,
                                       std::uint64_t expected,
                                       MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, core::Error> {
    if (!valid_cancellation(wait.cancellation))
        return std::unexpected{wait_error(MonitoringQ2CompactOperation::cancellation_contract)};
    if (expected > std::numeric_limits<std::uint32_t>::max())
        return std::unexpected{wait_error(MonitoringQ2CompactOperation::wait)};
    const auto snapshot = convert(default_system(), CLOCK_MONOTONIC, wait.deadline);
    if (snapshot.failure)
        return std::unexpected{monitoring_q2_materialize(*snapshot.failure)};
    auto* address =
        reinterpret_cast<std::uint32_t*>(const_cast<std::atomic<std::uint64_t>*>(&word));
    FutexCancellationCapsule capsule{const_cast<std::atomic<std::uint64_t>*>(&word)};
    const auto cancellation = wait.cancellation;
    if (cancellation.register_wake)
        cancellation.register_wake(cancellation.context, &capsule, futex_wake_capsule);
    const auto unregister = [&]() -> bool {
        return !cancellation.unregister_wake ||
               cancellation.unregister_wake(cancellation.context, &capsule) ==
                   MonitoringQ2CancellationUnregister::quiescent;
    };
    if (cancellation.requested && cancellation.requested(cancellation.context)) {
        if (!unregister())
            std::terminate();
        return std::unexpected{wait_error(MonitoringQ2CompactOperation::cancelled, ECANCELED)};
    }
    while (word.load(std::memory_order_acquire) == expected) {
        if (cancellation.requested && cancellation.requested(cancellation.context)) {
            if (!unregister())
                std::terminate();
            return std::unexpected{wait_error(MonitoringQ2CompactOperation::cancelled, ECANCELED)};
        }
        const auto rc = ::syscall(SYS_futex, address, FUTEX_WAIT_BITSET_PRIVATE,
                                  static_cast<std::uint32_t>(expected), &snapshot.absolute, nullptr,
                                  FUTEX_BITSET_MATCH_ANY);
        if (rc == 0 || errno == EAGAIN || errno == EINTR)
            continue;
        if (errno == ETIMEDOUT) {
            const bool cancelled =
                cancellation.requested && cancellation.requested(cancellation.context);
            if (!unregister())
                std::terminate();
            return std::unexpected{cancelled ? wait_error(MonitoringQ2CompactOperation::cancelled, ECANCELED)
                                             : wait_error(MonitoringQ2CompactOperation::timeout, ETIMEDOUT)};
        }
        const auto native = errno;
        if (!unregister())
            std::terminate();
        return std::unexpected{wait_error(MonitoringQ2CompactOperation::wait, native)};
    }
    const bool cancelled = cancellation.requested && cancellation.requested(cancellation.context);
    if (!unregister())
        std::terminate();
    if (cancelled)
        return std::unexpected{wait_error(MonitoringQ2CompactOperation::cancelled, ECANCELED)};
    return {};
}
void monitoring_q2_atomic_wake(std::atomic<std::uint32_t>& word) noexcept {
    word.notify_all();
    static_cast<void>(::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word),
                                FUTEX_WAKE_BITSET_PRIVATE, std::numeric_limits<int>::max(), nullptr,
                                nullptr, FUTEX_BITSET_MATCH_ANY));
}
void monitoring_q2_atomic_wake(std::atomic<std::uint64_t>& word) noexcept {
    word.notify_all();
    static_cast<void>(::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word),
                                FUTEX_WAKE_BITSET_PRIVATE, std::numeric_limits<int>::max(), nullptr,
                                nullptr, FUTEX_BITSET_MATCH_ANY));
}
} // namespace epycbench::monitoring::qualification::detail
