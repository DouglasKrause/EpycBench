#include "monitoring/host_q2_helper_provider.hpp"
#include "monitoring/host_q2_native_thread_owner.hpp"
#include <pthread.h>
#include <sched.h>
namespace epycbench::monitoring::qualification::detail {
namespace {
std::atomic<std::uintptr_t> live_helper_registry{};
auto provider_error(std::string classification, int os = 0) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.helper_provider"},
            .message = "Q2 helper provider failed",
            .operation = "monitoring_q2_helper_provider",
            .os_error = os ? std::optional{core::OsError{os, "pthread", "helper"}} : std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto compact(MonitoringQ2CompactOperation operation, const core::Error& value) noexcept
    -> MonitoringQ2CompactDiagnostic {
    return {operation, value.os_error ? value.os_error->value : 0};
}
} // namespace
struct MonitoringQ2HelperProvider::State {
    std::optional<MonitoringQ2NativeThreadOwner> thread;
    std::atomic<bool> parked{}, terminal{};
    std::atomic<std::uint32_t> start_gate{};
    bool registered{};
    std::array<std::optional<core::Error>, 4> errors;
    std::optional<Q2HelperProviderOperations> injected;
    std::uintptr_t injected_handle{};
    bool injected_active{};
    bool owns_global_registry{};
    std::optional<MonitoringQ2HelperCompactFailure> recovery_failure;
    void* entry_context{};
    void (*entry_function)(void*) noexcept {};
    static void run(void* raw, MonitoringQ2NativeThreadOwner::StopToken) noexcept {
        auto& self = *static_cast<State*>(raw);
        self.entry_function(self.entry_context);
        self.terminal.store(true, std::memory_order_release);
    }
};
MonitoringQ2HelperProvider::MonitoringQ2HelperProvider(std::unique_ptr<State> s) noexcept
    : state_{std::move(s)} {}
MonitoringQ2HelperProvider::MonitoringQ2HelperProvider(MonitoringQ2HelperProvider&&) noexcept =
    default;
MonitoringQ2HelperProvider::~MonitoringQ2HelperProvider() noexcept {
    if (state_ && ((state_->injected && state_->injected_active) || state_->thread))
        std::terminate();
}
auto MonitoringQ2HelperProvider::create() noexcept
    -> std::expected<MonitoringQ2HelperProvider, core::Error> {
    try {
        auto state = std::make_unique<State>();
        state->errors[0].emplace(provider_error("launch"));
        state->errors[1].emplace(provider_error("bind"));
        state->errors[2].emplace(provider_error("verify"));
        state->errors[3].emplace(provider_error("registry"));
        return MonitoringQ2HelperProvider{std::move(state)};
    } catch (...) {
        return std::unexpected{core::Error{}};
    }
}
auto MonitoringQ2HelperProvider::create(Q2HelperProviderOperations operations) noexcept
    -> std::expected<MonitoringQ2HelperProvider, core::Error> {
    if (!operations.create || !operations.await_parked || !operations.release_start ||
        !operations.set_affinity || !operations.get_affinity || !operations.get_policy ||
        !operations.registry_add || !operations.stop || !operations.wake || !operations.terminal ||
        !operations.join || !operations.close || !operations.registry_absent)
        return std::unexpected{provider_error("operations")};
    try {
        auto state = std::make_unique<State>();
        state->injected.emplace(operations);
        state->errors[0].emplace(provider_error("launch"));
        state->errors[1].emplace(provider_error("bind"));
        state->errors[2].emplace(provider_error("verify"));
        state->errors[3].emplace(provider_error("registry"));
        return MonitoringQ2HelperProvider{std::move(state)};
    } catch (...) {
        return std::unexpected{provider_error("construction")};
    }
}
auto MonitoringQ2HelperProvider::operations() noexcept -> Q2HelperOperations {
    if (!state_)
        std::terminate();
    if (state_->injected) {
        return {
            state_.get(),
            [](void* raw, void* context, void (*entry)(void*) noexcept,
               MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2HelperLaunchOutcome {
                auto& state = *static_cast<State*>(raw);
                auto& o = *state.injected;
                auto created = o.create(o.context, context, entry);
                if (!created)
                    return {{}, std::move(created.error())};
                state.injected_handle = *created;
                state.injected_active = true;
                auto parked = o.await_parked(o.context, *created, wait);
                if (!parked) {
                    MonitoringQ2HelperCompactFailure failure{
                        compact(MonitoringQ2CompactOperation::wait,parked.error()),{}};
                    state.recovery_failure.emplace(std::move(failure));
                    return {*created, std::move(parked.error())};
                }
                return {*created, {}};
            },
            // Injected providers are deterministic cooperative schedulers: returning from
            // this wait yields the entry capsule to the test scheduler. Production below
            // retains the blocking atomic-wait implementation.
            [](void* raw, const std::atomic<std::uint32_t>*, std::uint32_t,
               MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
                return std::unexpected{*static_cast<State*>(raw)->errors[0]};
            },
            [](void*, std::atomic<std::uint32_t>* word) noexcept { word->notify_all(); },
            [](void* raw, std::uintptr_t handle, core::LinuxCpuId cpu) noexcept {
                auto& state = *static_cast<State*>(raw);
                return state.injected->set_affinity(state.injected->context, handle, cpu);
            },
            [](void* raw, std::uintptr_t handle,
               core::LinuxCpuId cpu) noexcept -> Q2LifecycleResult {
                auto& state = *static_cast<State*>(raw);
                auto policy = state.injected->get_policy(state.injected->context, handle);
                if (!policy)
                    return std::unexpected{std::move(policy.error())};
                if (*policy != SCHED_OTHER)
                    return std::unexpected{*state.errors[2]};
                auto affinity = state.injected->get_affinity(state.injected->context, handle);
                if (!affinity)
                    return std::unexpected{std::move(affinity.error())};
                if (*affinity != core::CpuSet{cpu})
                    return std::unexpected{*state.errors[2]};
                return {};
            },
            [](void* raw, std::uintptr_t h) noexcept -> Q2LifecycleResult {
                auto& s = *static_cast<State*>(raw);
                std::uintptr_t empty{};
                if (!live_helper_registry.compare_exchange_strong(empty, h))
                    return Q2LifecycleResult{std::unexpected{std::move(*s.errors[3])}};
                s.owns_global_registry = true;
                auto result = s.injected->registry_add(s.injected->context, h);
                if (!result) {
                    live_helper_registry.store(0);
                    s.owns_global_registry = false;
                }
                return result;
            },
            [](void* raw, std::uintptr_t h) noexcept {
                auto& s = *static_cast<State*>(raw);
                return s.injected->stop(s.injected->context, h);
            },
            [](void* raw, std::uintptr_t h) noexcept {
                auto& s = *static_cast<State*>(raw);
                return s.injected->wake(s.injected->context, h);
            },
            [](void* raw, std::uintptr_t h, MonitoringQ2DeadlineCancellationContext wait) noexcept {
                auto& s = *static_cast<State*>(raw);
                return s.injected->terminal(s.injected->context, h, wait);
            },
            [](void* raw, std::uintptr_t h, MonitoringQ2DeadlineCancellationContext wait) noexcept {
                auto& s = *static_cast<State*>(raw);
                auto result=s.injected->join(s.injected->context,h,wait);
                if(!result)return std::expected<void,MonitoringQ2CompactDiagnostic>{std::unexpected{compact(MonitoringQ2CompactOperation::join,result.error())}};
                return std::expected<void,MonitoringQ2CompactDiagnostic>{};
            },
            [](void* raw, std::uintptr_t h) noexcept {
                auto& s = *static_cast<State*>(raw);
                auto r = s.injected->close(s.injected->context, h);
                if (r) {
                    s.injected_active = false;
                    if (s.owns_global_registry) {
                        auto owned = h;
                        static_cast<void>(live_helper_registry.compare_exchange_strong(
                            owned, 0, std::memory_order_acq_rel));
                        s.owns_global_registry = false;
                    }
                }
                return r;
            },
            [](void* raw, std::uintptr_t h, MonitoringQ2DeadlineCancellationContext wait) noexcept {
                auto& s = *static_cast<State*>(raw);
                return s.injected->terminal(s.injected->context, h, wait);
            },
            [](void* raw, std::uintptr_t h) noexcept -> std::expected<bool, core::Error> {
                auto& s = *static_cast<State*>(raw);
                auto absent = s.injected->registry_absent(s.injected->context, h);
                if (!absent)
                    return absent;
                if (*absent && !s.injected_active)
                    s.recovery_failure.reset();
                return *absent && (!s.owns_global_registry ||
                                   live_helper_registry.load(std::memory_order_acquire) == 0);
            },
            [](void* raw, std::uintptr_t h) noexcept {
                auto& s = *static_cast<State*>(raw);
                auto result=s.injected->release_start(s.injected->context,h);
                if(!result)return std::expected<void,MonitoringQ2CompactDiagnostic>{std::unexpected{compact(MonitoringQ2CompactOperation::release,result.error())}};
                return std::expected<void,MonitoringQ2CompactDiagnostic>{};
            }};
    }
    return {
        state_.get(),
        [](void* raw, void* context, void (*entry)(void*) noexcept,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2HelperLaunchOutcome {
            auto& s = *static_cast<State*>(raw);
            s.entry_context = context;
            s.entry_function = entry;
            auto owner = MonitoringQ2NativeThreadOwner::create_parked(&s, State::run, wait);
            if (!owner) {
                auto failure = std::move(owner).take_failure();
                auto primary = std::move(*s.errors[0]);
                auto recovery = std::move(owner).recovery_owner();
                if (recovery)
                    s.thread.emplace(std::move(*recovery));
                MonitoringQ2HelperCompactFailure retained{failure.primary,{}};
                for (std::size_t i{}; i < failure.cleanup.size(); ++i)
                    retained.cleanup.append(failure.cleanup[i]);
                if (failure.cleanup.overflowed())
                    retained.cleanup.note_overflow();
                if (retained.cleanup.size() != 0U || recovery)
                    s.recovery_failure.emplace(std::move(retained));
                return {recovery ? std::optional<std::uintptr_t>{1} : std::nullopt,
                        std::move(primary)};
            }
            s.thread.emplace(std::move(owner).owner());
            s.parked.store(true);
            return {1, {}};
        },
        [](void*, const std::atomic<std::uint32_t>* word, std::uint32_t expected,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            return monitoring_q2_bounded_atomic_wait(*word, expected, wait);
        },
        [](void*, std::atomic<std::uint32_t>* word) noexcept { monitoring_q2_atomic_wake(*word); },
        [](void* raw, std::uintptr_t, core::LinuxCpuId cpu) noexcept -> Q2LifecycleResult {
            auto& s = *static_cast<State*>(raw);
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu.value(), &set);
            const int rc = pthread_setaffinity_np(s.thread->native_handle(), sizeof(set), &set);
            if (rc) {
                return std::unexpected{provider_error("affinity_syscall", rc)};
            }
            return {};
        },
        [](void* raw, std::uintptr_t, core::LinuxCpuId cpu) noexcept -> Q2LifecycleResult {
            auto& s = *static_cast<State*>(raw);
            int policy{};
            sched_param param{};
            const int policy_rc = pthread_getschedparam(s.thread->native_handle(), &policy, &param);
            if (policy_rc != 0)
                return std::unexpected{provider_error("scheduler_syscall", policy_rc)};
            if (policy != SCHED_OTHER)
                return std::unexpected{provider_error("scheduler_mismatch")};
            cpu_set_t set;
            CPU_ZERO(&set);
            const int affinity_rc =
                pthread_getaffinity_np(s.thread->native_handle(), sizeof(set), &set);
            if (affinity_rc != 0)
                return std::unexpected{provider_error("affinity_syscall", affinity_rc)};
            if (!CPU_ISSET(cpu.value(), &set) || CPU_COUNT(&set) != 1)
                return std::unexpected{provider_error("affinity_mismatch")};
            return {};
        },
        [](void* raw, std::uintptr_t) noexcept -> Q2LifecycleResult {
            auto& s = *static_cast<State*>(raw);
            if (s.registered) {
                return std::unexpected{std::move(*s.errors[3])};
            }
            std::uintptr_t empty{};
            if (!live_helper_registry.compare_exchange_strong(empty, 1U))
                return std::unexpected{std::move(*s.errors[3])};
            s.registered = true;
            return {};
        },
        [](void* raw, std::uintptr_t) noexcept -> Q2LifecycleResult {
            auto& s = *static_cast<State*>(raw);
            if (s.thread) {
                s.thread->request_stop();
            }
            return {};
        },
        [](void*, std::uintptr_t) noexcept -> Q2LifecycleResult { return {}; },
        [](void*, std::uintptr_t,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult { return {}; },
        [](void* raw, std::uintptr_t,
           MonitoringQ2DeadlineCancellationContext wait) noexcept
            -> std::expected<void,MonitoringQ2CompactDiagnostic> {
            auto& s = *static_cast<State*>(raw);
            if (!s.thread)
                return std::expected<void,MonitoringQ2CompactDiagnostic>{};
            auto result=s.thread->timed_join(wait);
            auto cleanup=s.thread->take_cleanup_diagnostics();
            if (cleanup.size()!=0U || cleanup.overflowed()) {
                if (!s.recovery_failure)
                    s.recovery_failure.emplace(MonitoringQ2HelperCompactFailure{
                        cleanup.size()!=0U?cleanup[0]:MonitoringQ2CompactDiagnostic{
                            MonitoringQ2CompactOperation::overflow,0}, {}});
                for (std::size_t index=1U;index<cleanup.size();++index)
                    s.recovery_failure->cleanup.append(cleanup[index]);
                if(cleanup.overflowed())s.recovery_failure->cleanup.note_overflow();
            }
            return result;
        },
        [](void* raw, std::uintptr_t) noexcept -> Q2LifecycleResult {
            auto& s = *static_cast<State*>(raw);
            if (s.thread && s.thread->active())
                return std::unexpected{std::move(*s.errors[2])};
            s.thread.reset();
            s.registered = false;
            std::uintptr_t owned{1U};
            static_cast<void>(
                live_helper_registry.compare_exchange_strong(owned, 0, std::memory_order_acq_rel));
            return {};
        },
        [](void* raw, std::uintptr_t,
           MonitoringQ2DeadlineCancellationContext) noexcept -> Q2LifecycleResult {
            return static_cast<State*>(raw)->terminal.load()
                       ? Q2LifecycleResult{}
                       : Q2LifecycleResult{std::unexpected{*static_cast<State*>(raw)->errors[2]}};
        },
        [](void* raw, std::uintptr_t) noexcept -> std::expected<bool, core::Error> {
            return !static_cast<State*>(raw)->registered &&
                   live_helper_registry.load(std::memory_order_acquire) == 0;
        },
        [](void* raw, std::uintptr_t) noexcept
            -> std::expected<void,MonitoringQ2CompactDiagnostic> {
            auto& s = *static_cast<State*>(raw);
            if (!s.thread)
                return std::unexpected{MonitoringQ2CompactDiagnostic{MonitoringQ2CompactOperation::release,0}};
            return s.thread->release_start();
        }};
}

auto MonitoringQ2HelperProvider::retire_recovery(
    MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, MonitoringQ2HelperCompactFailure> {
    if (!state_)
        std::terminate();
    const auto append_failure = [&](core::Error value) noexcept {
        const auto diagnostic=compact(MonitoringQ2CompactOperation::retire,value);
        if (!state_->recovery_failure)
            state_->recovery_failure.emplace(MonitoringQ2HelperCompactFailure{diagnostic,{}});
        else state_->recovery_failure->cleanup.append(diagnostic);
    };
    if (state_->thread) {
        state_->thread->request_stop();
        auto joined = state_->thread->timed_join(wait);
        if (!joined) {
            if(!state_->recovery_failure)
                state_->recovery_failure.emplace(MonitoringQ2HelperCompactFailure{joined.error(),{}});
            else state_->recovery_failure->cleanup.append(joined.error());
        }
        auto cleanup=state_->thread->take_cleanup_diagnostics();
        for(std::size_t index{};index<cleanup.size();++index) {
            if(!state_->recovery_failure)
                state_->recovery_failure.emplace(MonitoringQ2HelperCompactFailure{cleanup[index],{}});
            else state_->recovery_failure->cleanup.append(cleanup[index]);
        }
        if(cleanup.overflowed()&&state_->recovery_failure)
            state_->recovery_failure->cleanup.note_overflow();
        if (!state_->thread->active())
            state_->thread.reset();
    }
    if (state_->injected && state_->injected_active) {
        auto& operations = *state_->injected;
        const auto record = [&](Q2LifecycleResult result) noexcept {
            if (!result)
                append_failure(std::move(result.error()));
            return result.has_value();
        };
        record(operations.stop(operations.context, state_->injected_handle));
        record(operations.wake(operations.context, state_->injected_handle));
        record(operations.terminal(operations.context, state_->injected_handle, wait));
        const bool joined =
            record(operations.join(operations.context, state_->injected_handle, wait));
        const bool closed = record(operations.close(operations.context, state_->injected_handle));
        auto absent = operations.registry_absent(operations.context, state_->injected_handle);
        const bool absent_ok = absent && *absent;
        if (!absent)
            append_failure(std::move(absent.error()));
        if (closed && state_->owns_global_registry) {
            auto owned = state_->injected_handle;
            static_cast<void>(
                live_helper_registry.compare_exchange_strong(owned, 0, std::memory_order_acq_rel));
            state_->owns_global_registry = false;
        }
        state_->injected_active = !(joined && closed && absent_ok);
    }
    const bool active = state_->thread.has_value() || state_->injected_active;
    if (state_->recovery_failure) {
        auto reported = *state_->recovery_failure;
        if (!active)
            state_->recovery_failure.reset();
        return std::unexpected{std::move(reported)};
    }
    return {};
}
} // namespace epycbench::monitoring::qualification::detail
