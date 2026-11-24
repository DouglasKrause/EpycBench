#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <optional>
#include <pthread.h>
#include <signal.h>

namespace epycbench::platform_linux::detail {

enum class ControllerSignal { none, interrupt, terminate };
enum class SignalScopeStage {
    admission, identity, block, query_interrupt, query_terminate,
    install_interrupt, install_terminate, restore_mask,
    restore_interrupt, restore_terminate
};
enum class SignalScopeReason {
    invalid_state, invalid_operations, already_used, wrong_process, wrong_thread,
    initially_blocked, nondefault_disposition, native_failure, malformed_result
};
inline constexpr unsigned signal_residual_interrupt = 1U;
inline constexpr unsigned signal_residual_terminate = 2U;
inline constexpr unsigned signal_residual_mask = 4U;
inline constexpr unsigned signal_residual_identity = 8U;
struct SignalScopeClose {
    ControllerSignal observed{ControllerSignal::none};
    unsigned residual{};
};
struct SignalScopeDiagnostic {
    SignalScopeStage stage{};
    SignalScopeReason reason{};
    int error_number{};
};
struct SignalScopeFailure {
    SignalScopeDiagnostic primary;
    std::array<SignalScopeDiagnostic, 3> cleanup{};
    std::size_t cleanup_count{};
    SignalScopeClose facts{};
};
struct SignalActionResult { int value; int error_number; };
struct SignalScopeOperations {
    void* context;
    int (*process_id)(void*) noexcept;
    pthread_t (*thread_self)(void*) noexcept;
    bool (*thread_equal)(void*, pthread_t, pthread_t) noexcept;
    int (*mask)(void*, int, const sigset_t*, sigset_t*) noexcept;
    SignalActionResult (*action)(void*, int, const struct sigaction*, struct sigaction*) noexcept;
};
// Private isolated state for fake operations; native production state is never reset.
struct SignalScopeState {
    std::atomic<bool> used{false};
    std::atomic<int> first{0};
    std::atomic<unsigned> residual{0};
};
class SignalScopeAccess;
class ControllerSignalScope final {
  public:
    ControllerSignalScope() noexcept = default;
    ControllerSignalScope(const ControllerSignalScope&) = delete;
    auto operator=(const ControllerSignalScope&) -> ControllerSignalScope& = delete;
    ControllerSignalScope(ControllerSignalScope&&) = delete;
    auto operator=(ControllerSignalScope&&) -> ControllerSignalScope& = delete;
    ~ControllerSignalScope() noexcept;
    [[nodiscard]] auto acquire() noexcept -> std::expected<void, SignalScopeFailure>;
    [[nodiscard]] auto acquire_for_package() noexcept -> std::expected<void, SignalScopeFailure>;
    // Nonconsuming admission on the installing process/thread; no signal or mask operations.
    [[nodiscard]] auto validate() const noexcept -> std::expected<void, SignalScopeFailure>;
    [[nodiscard]] auto observe() const noexcept -> ControllerSignal;
    [[nodiscard]] auto close() noexcept -> std::expected<SignalScopeClose, SignalScopeFailure>;

  private:
    enum class State { fresh, active, terminal };
    [[nodiscard]] auto snapshot() const noexcept -> SignalScopeClose;
    [[nodiscard]] auto acquire_native(SignalScopeState&, void (*handler)(int), bool reusable) noexcept
        -> std::expected<void, SignalScopeFailure>;
    void restore(std::optional<SignalScopeFailure>&, bool restore_mask) noexcept;
    void publish_residue() noexcept;
    State state_{State::fresh};
    SignalScopeState* process_state_{};
    SignalScopeOperations operations_{};
    int process_{};
    pthread_t thread_{};
    sigset_t pair_{};
    sigset_t original_mask_{};
    struct sigaction interrupt_{};
    struct sigaction terminate_{};
    bool interrupt_dirty_{};
    bool terminate_dirty_{};
    bool mask_dirty_{};
    bool native_claim_{};
    bool reusable_{};
    friend class SignalScopeAccess;
};
class SignalScopeAccess final {
  public:
    [[nodiscard]] static auto acquire(ControllerSignalScope&, SignalScopeState&,
                                      SignalScopeOperations, void (*handler)(int) = nullptr) noexcept
        -> std::expected<void, SignalScopeFailure>;
    static void deliver(SignalScopeState&, int signal) noexcept;
};
} // namespace epycbench::platform_linux::detail
