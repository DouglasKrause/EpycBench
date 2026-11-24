#include "platform_linux/controller_signal_scope.hpp"

#include <cerrno>
#include <unistd.h>

namespace epycbench::platform_linux::detail {
namespace {
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
constinit SignalScopeState production_state;
constinit SignalScopeState package_state;
constinit std::atomic<bool> native_owner{};

void latch(SignalScopeState& state, int signal) noexcept {
    const int reason = signal == SIGINT ? 1 : signal == SIGTERM ? 2 : 0;
    if (reason == 0) return;
    int expected{};
    static_cast<void>(state.first.compare_exchange_strong(
        expected, reason, std::memory_order_relaxed, std::memory_order_relaxed));
}
extern "C" void controller_signal_handler(int signal) noexcept {
    latch(production_state, signal);
}
extern "C" void package_controller_signal_handler(int signal) noexcept {
    latch(package_state, signal);
}
auto native_pid(void*) noexcept -> int { return static_cast<int>(::getpid()); }
auto native_thread(void*) noexcept -> pthread_t { return ::pthread_self(); }
auto native_equal(void*, pthread_t left, pthread_t right) noexcept -> bool {
    return ::pthread_equal(left, right) != 0;
}
auto native_mask(void*, int how, const sigset_t* set, sigset_t* previous) noexcept -> int {
    return ::pthread_sigmask(how, set, previous);
}
auto native_action(void*, int signal, const struct sigaction* action,
                   struct sigaction* previous) noexcept -> SignalActionResult {
    const auto result = ::sigaction(signal, action, previous);
    return {result, result == -1 ? errno : 0};
}
auto reason(const SignalScopeState* state) noexcept -> ControllerSignal {
    if (state == nullptr) return ControllerSignal::none;
    const auto value = state->first.load(std::memory_order_relaxed);
    return value == 1 ? ControllerSignal::interrupt :
           value == 2 ? ControllerSignal::terminate : ControllerSignal::none;
}
auto failure(SignalScopeStage stage, SignalScopeReason why, int number = 0) noexcept
    -> SignalScopeFailure {
    return {.primary = {stage, why, number}};
}
void record(std::optional<SignalScopeFailure>& result, SignalScopeDiagnostic diagnostic) noexcept {
    if (!result) result = SignalScopeFailure{.primary = diagnostic};
    else if (result->cleanup_count < result->cleanup.size())
        result->cleanup[result->cleanup_count++] = diagnostic;
}
auto action_diagnostic(SignalScopeStage stage, SignalActionResult result) noexcept
    -> std::optional<SignalScopeDiagnostic> {
    if (result.value == 0 && result.error_number == 0) return std::nullopt;
    return SignalScopeDiagnostic{stage,
        result.value == -1 && result.error_number > 0 ? SignalScopeReason::native_failure :
                                                        SignalScopeReason::malformed_result,
        result.error_number};
}
auto mask_diagnostic(SignalScopeStage stage, int result) noexcept
    -> std::optional<SignalScopeDiagnostic> {
    if (result == 0) return std::nullopt;
    return SignalScopeDiagnostic{stage, result > 0 ? SignalScopeReason::native_failure :
                                                   SignalScopeReason::malformed_result,
                                 result > 0 ? result : 0};
}
} // namespace

void SignalScopeAccess::deliver(SignalScopeState& state, int signal) noexcept {
    latch(state, signal);
}
auto ControllerSignalScope::acquire() noexcept -> std::expected<void, SignalScopeFailure> {
    return acquire_native(production_state,controller_signal_handler,false);
}
auto ControllerSignalScope::acquire_for_package() noexcept -> std::expected<void, SignalScopeFailure> {
    return acquire_native(package_state,package_controller_signal_handler,true);
}
auto ControllerSignalScope::acquire_native(SignalScopeState& state,void (*handler)(int),bool reusable) noexcept
    -> std::expected<void, SignalScopeFailure> {
    if (state_!=State::fresh)
        return std::unexpected{failure(SignalScopeStage::admission,SignalScopeReason::invalid_state)};
    bool unowned{};
    if (!native_owner.compare_exchange_strong(unowned,true,std::memory_order_acquire))
        return std::unexpected{failure(SignalScopeStage::admission,SignalScopeReason::already_used)};
    const auto acquired=SignalScopeAccess::acquire(*this,state,
        {nullptr,native_pid,native_thread,native_equal,native_mask,native_action},handler);
    if (acquired) {
        native_claim_=true;
        reusable_=reusable;
    } else if (acquired.error().facts.residual==0) {
        if (reusable) state.used.store(false,std::memory_order_relaxed);
        native_owner.store(false,std::memory_order_release);
    }
    return acquired;
}
auto ControllerSignalScope::snapshot() const noexcept -> SignalScopeClose {
    return {reason(process_state_),
        (interrupt_dirty_ ? signal_residual_interrupt : 0U) |
        (terminate_dirty_ ? signal_residual_terminate : 0U) |
        (mask_dirty_ ? signal_residual_mask : 0U)};
}
void ControllerSignalScope::publish_residue() noexcept {
    if (process_state_ != nullptr)
        process_state_->residual.fetch_or(snapshot().residual, std::memory_order_relaxed);
}
void ControllerSignalScope::restore(std::optional<SignalScopeFailure>& result,
                                     bool restore_mask) noexcept {
    if (terminate_dirty_) {
        const auto error = action_diagnostic(SignalScopeStage::restore_terminate,
            operations_.action(operations_.context, SIGTERM, &terminate_, nullptr));
        if (error) record(result, *error);
        else terminate_dirty_ = false;
    }
    if (interrupt_dirty_) {
        const auto error = action_diagnostic(SignalScopeStage::restore_interrupt,
            operations_.action(operations_.context, SIGINT, &interrupt_, nullptr));
        if (error) record(result, *error);
        else interrupt_dirty_ = false;
    }
    if (restore_mask && mask_dirty_) {
        const auto error = mask_diagnostic(SignalScopeStage::restore_mask,
            operations_.mask(operations_.context, SIG_SETMASK, &original_mask_, nullptr));
        if (error) record(result, *error);
        else mask_dirty_ = false;
    }
    publish_residue();
    if (result) result->facts = snapshot();
}
auto SignalScopeAccess::acquire(ControllerSignalScope& owner, SignalScopeState& state,
                               SignalScopeOperations operations,void (*handler)(int)) noexcept
    -> std::expected<void, SignalScopeFailure> {
    if (owner.state_ != ControllerSignalScope::State::fresh)
        return std::unexpected{failure(SignalScopeStage::admission, SignalScopeReason::invalid_state)};
    owner.state_ = ControllerSignalScope::State::terminal;
    bool unused = false;
    if (!state.used.compare_exchange_strong(unused, true, std::memory_order_relaxed))
        return std::unexpected{failure(SignalScopeStage::admission, SignalScopeReason::already_used)};
    owner.process_state_ = &state;
    if (!operations.process_id || !operations.thread_self || !operations.thread_equal ||
        !operations.mask || !operations.action)
        return std::unexpected{failure(SignalScopeStage::admission, SignalScopeReason::invalid_operations)};
    owner.operations_ = operations;
    owner.process_ = operations.process_id(operations.context);
    if (owner.process_ <= 0)
        return std::unexpected{failure(SignalScopeStage::identity, SignalScopeReason::malformed_result)};
    owner.thread_ = operations.thread_self(operations.context);
    static_cast<void>(::sigemptyset(&owner.pair_));
    static_cast<void>(::sigaddset(&owner.pair_, SIGINT));
    static_cast<void>(::sigaddset(&owner.pair_, SIGTERM));
    auto error = mask_diagnostic(SignalScopeStage::block,
        operations.mask(operations.context, SIG_BLOCK, &owner.pair_, &owner.original_mask_));
    if (error) {
        if (error->reason == SignalScopeReason::malformed_result) {
            // An invalid operation result cannot establish that the mask was unchanged.
            // The prior-mask output is untrusted, so no restoration is possible.
            owner.mask_dirty_ = true;
            owner.publish_residue();
        }
        return std::unexpected{SignalScopeFailure{.primary = *error, .facts = owner.snapshot()}};
    }
    owner.mask_dirty_ = true;
    std::optional<SignalScopeFailure> result;
    const auto reject = [&](SignalScopeDiagnostic diagnostic, bool restore_mask = true)
        -> std::expected<void, SignalScopeFailure> {
        record(result, diagnostic);
        owner.restore(result, restore_mask);
        return std::unexpected{*result};
    };
    if (::sigismember(&owner.original_mask_, SIGINT) != 0 ||
        ::sigismember(&owner.original_mask_, SIGTERM) != 0)
        return reject({SignalScopeStage::admission, SignalScopeReason::initially_blocked, 0});
    error = action_diagnostic(SignalScopeStage::query_interrupt,
        operations.action(operations.context, SIGINT, nullptr, &owner.interrupt_));
    if (error) return reject(*error);
    error = action_diagnostic(SignalScopeStage::query_terminate,
        operations.action(operations.context, SIGTERM, nullptr, &owner.terminate_));
    if (error) return reject(*error);
    if (owner.interrupt_.sa_handler != SIG_DFL || owner.terminate_.sa_handler != SIG_DFL)
        return reject({SignalScopeStage::admission, SignalScopeReason::nondefault_disposition, 0});
    struct sigaction action{};
    action.sa_handler = handler ? handler : controller_signal_handler;
    action.sa_mask = owner.pair_;
    action.sa_flags = 0;
    owner.interrupt_dirty_ = true;
    error = action_diagnostic(SignalScopeStage::install_interrupt,
        operations.action(operations.context, SIGINT, &action, nullptr));
    if (error) return reject(*error);
    owner.terminate_dirty_ = true;
    error = action_diagnostic(SignalScopeStage::install_terminate,
        operations.action(operations.context, SIGTERM, &action, nullptr));
    if (error) return reject(*error);
    error = mask_diagnostic(SignalScopeStage::restore_mask,
        operations.mask(operations.context, SIG_SETMASK, &owner.original_mask_, nullptr));
    if (error) return reject(*error, false);
    owner.mask_dirty_ = false;
    owner.state_ = ControllerSignalScope::State::active;
    return {};
}
auto ControllerSignalScope::observe() const noexcept -> ControllerSignal {
    return state_ == State::active ? reason(process_state_) : ControllerSignal::none;
}
auto ControllerSignalScope::validate() const noexcept -> std::expected<void, SignalScopeFailure> {
    if (state_ != State::active)
        return std::unexpected{failure(SignalScopeStage::admission, SignalScopeReason::invalid_state)};
    const auto reject_identity = [&](SignalScopeReason why) {
        auto result = failure(SignalScopeStage::identity, why);
        result.facts = snapshot();
        return std::unexpected{result};
    };
    if (operations_.process_id(operations_.context) != process_)
        return reject_identity(SignalScopeReason::wrong_process);
    if (!operations_.thread_equal(operations_.context, thread_,
                                  operations_.thread_self(operations_.context)))
        return reject_identity(SignalScopeReason::wrong_thread);
    return {};
}
auto ControllerSignalScope::close() noexcept -> std::expected<SignalScopeClose, SignalScopeFailure> {
    auto valid = validate();
    if (!valid) return std::unexpected{valid.error()};
    state_ = State::terminal;
    std::optional<SignalScopeFailure> result;
    const auto block = mask_diagnostic(SignalScopeStage::block,
        operations_.mask(operations_.context, SIG_BLOCK, &pair_, nullptr));
    mask_dirty_ = true;
    if (block) record(result, *block);
    const auto observed = reason(process_state_);
    restore(result, true);
    if (result) {
        result->facts.observed = observed;
        return std::unexpected{*result};
    }
    if (native_claim_) {
        if (reusable_) {
            process_state_->first.store(0,std::memory_order_relaxed);
            process_state_->used.store(false,std::memory_order_relaxed);
        }
        native_owner.store(false,std::memory_order_release);
    }
    return SignalScopeClose{observed, 0U};
}
ControllerSignalScope::~ControllerSignalScope() noexcept {
    if (state_ != State::active) return;
    static_cast<void>(close());
    if (state_ == State::active) {
        // Wrong process/thread: never mutate another thread's mask or retry.
        mask_dirty_ = true;
        publish_residue();
        process_state_->residual.fetch_or(signal_residual_identity, std::memory_order_relaxed);
        state_ = State::terminal;
    }
}
} // namespace epycbench::platform_linux::detail
