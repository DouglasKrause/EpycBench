#pragma once
#include "epycbench/platform_linux/thread_identity.hpp"
#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_controlled_generation_protocol.hpp"
#include "monitoring/host_q2_native_thread_owner.hpp"
#include <array>
#include <atomic>
#include <expected>
#include <memory>
#include <span>
namespace epycbench::monitoring::qualification::detail {
struct MonitoringQ2ControlledEventSessionTestAccess;
enum class Q2EventStatus : std::uint8_t {
    ok,
    stale,
    missing,
    duplicate,
    crossed,
    invalid,
    cancelled,
    deadline,
    ceiling,
    helper_failure
};
using Q2LifecycleResult = std::expected<void, core::Error>;
struct Q2ControlledFailure {
    static constexpr std::size_t maximum_cleanup = 16;
    Q2ControlledFailure() noexcept = default;
    explicit Q2ControlledFailure(core::Error value) noexcept
        : primary{std::move(value)}, has_primary{true} {}
    core::Error primary;
    bool has_primary{};
    std::array<std::optional<core::Error>, maximum_cleanup> cleanup{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
    MonitoringQ2CompactLedger<16> compact_cleanup;
};
inline auto operator==(const std::expected<void, Q2ControlledFailure>& value,
                       Q2EventStatus status) noexcept -> bool {
    return status == Q2EventStatus::ok && value.has_value();
}
struct Q2TargetOperations {
    void* context{};
    const Q2TargetSnapshot* (*snapshot)(void*, std::size_t) noexcept {};
    void (*attach_protocol)(void*, Q2ControlledGenerationProtocol*) noexcept {};
    Q2LifecycleResult (*revalidate)(void*, std::size_t,
                                    MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*begin)(void*, std::size_t, std::uint64_t,
                               MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*release)(void*, std::size_t, std::uint64_t,
                                 MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*stop)(void*, std::size_t, std::uint64_t,
                              MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*drain_terminal)(void*, std::size_t, std::uint64_t,
                                        MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*repark)(void*, std::size_t, std::uint64_t,
                                MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*quiesce)(void*, std::size_t,
                                 MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*begin_voluntary)(void*, std::size_t, std::uint64_t,
                                         MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*release_to_wait)(void*, std::size_t, std::uint64_t,
                                         MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*wake_voluntary)(void*, std::size_t, std::uint64_t,
                                        MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*finish_voluntary)(void*, std::size_t, std::uint64_t,
                                          MonitoringQ2DeadlineCancellationContext) noexcept {};
};
struct Q2HelperLaunchOutcome final {
    std::optional<std::uintptr_t> handle;
    std::optional<core::Error> failure;
    explicit operator bool() const noexcept { return handle.has_value() && !failure; }
};
struct Q2HelperOperations {
    using Handle = std::uintptr_t;
    void* context{};
    Q2HelperLaunchOutcome (*launch)(void*, void*, void (*)(void*) noexcept,
                                    MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*wait)(void*, const std::atomic<std::uint32_t>*, std::uint32_t,
                              MonitoringQ2DeadlineCancellationContext) noexcept {};
    void (*wake)(void*, std::atomic<std::uint32_t>*) noexcept {};
    Q2LifecycleResult (*bind_cpu)(void*, Handle, core::LinuxCpuId) noexcept {};
    Q2LifecycleResult (*verify_other_affinity)(void*, Handle, core::LinuxCpuId) noexcept {};
    Q2LifecycleResult (*registry_enter)(void*, Handle) noexcept {};
    Q2LifecycleResult (*request_stop)(void*, Handle) noexcept {};
    Q2LifecycleResult (*unblock)(void*, Handle) noexcept {};
    Q2LifecycleResult (*drain_terminal)(void*, Handle,
                                        MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<void,MonitoringQ2CompactDiagnostic> (*join)(void*, Handle, MonitoringQ2DeadlineCancellationContext) noexcept {};
    Q2LifecycleResult (*close)(void*, Handle) noexcept {};
    Q2LifecycleResult (*verify_terminal)(void*, Handle,
                                         MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<bool, core::Error> (*registry_absent)(void*, Handle) noexcept {};
    std::expected<void,MonitoringQ2CompactDiagnostic> (*release_start)(void*, Handle) noexcept {};
};
struct Q2GenerationResult {
    std::uint64_t generation{};
    Q2Winner winner{};
    bool target_terminal{}, helper_terminal{};
};
class MonitoringQ2ControlledEventSession final {
  public:
    static auto create(std::span<const MonitoringQ2WorkerBinding>, std::size_t, Q2TargetOperations,
                       Q2HelperOperations) noexcept
        -> std::expected<MonitoringQ2ControlledEventSession, Q2ControlledFailure>;
    MonitoringQ2ControlledEventSession(MonitoringQ2ControlledEventSession&&) noexcept;
    MonitoringQ2ControlledEventSession(const MonitoringQ2ControlledEventSession&) = delete;
    ~MonitoringQ2ControlledEventSession() noexcept;
    auto begin(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept -> Q2EventStatus;
    auto ready(std::uint64_t, bool, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto release_target(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto target_running(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto release_helper(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto compete(std::uint64_t, Q2Winner, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto stop(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept -> Q2EventStatus;
    auto terminal(std::uint64_t, bool, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto finish(std::uint64_t, Q2GenerationResult*,
                MonitoringQ2DeadlineCancellationContext) noexcept -> Q2EventStatus;
    auto retire(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, Q2ControlledFailure>;
    auto retirement_retryable() const noexcept -> bool { return active_; }
    auto begin_voluntary(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto finish_voluntary_and_prepare_helper(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, Q2ControlledFailure>;
    auto voluntary_ready(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto release_wait(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto wake(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept -> Q2EventStatus;
    auto resumed(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept -> Q2EventStatus;
    auto finish_voluntary(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2EventStatus;
    auto abort(std::uint64_t, core::Error, MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, Q2ControlledFailure>;
    auto target() const noexcept -> const Q2TargetSnapshot& { return target_; }

  private:
    struct LoopState;
    static void helper_entry(void*) noexcept;
    MonitoringQ2ControlledEventSession(Q2TargetSnapshot, std::size_t, Q2TargetOperations,
                                       Q2HelperOperations, Q2HelperOperations::Handle, core::Error,
                                       core::Error, std::unique_ptr<LoopState>) noexcept;
    enum class Phase : std::uint8_t {
        idle,
        parking,
        target_released,
        target_running,
        helper_released,
        stopping,
        terminal,
        voluntary_parking,
        voluntary_ready,
        voluntary_waiting,
        voluntary_woken,
        voluntary_resumed,
        retired
    };
    Q2TargetSnapshot target_;
    Q2TargetOperations target_ops_{};
    Q2HelperOperations helper_ops_{};
    Q2HelperOperations::Handle helper_{};
    core::Error overflow_error_;
    core::Error binding_error_;
    std::optional<core::Error> pending_error_;
    std::uint64_t generation_{};
    std::size_t target_index_{};
    std::atomic<Q2Winner> winner_{Q2Winner::undecided};
    Phase phase_{Phase::idle};
    bool target_ready_{}, helper_ready_{}, target_terminal_{}, helper_terminal_{}, active_{true},
        target_retired_{}, helper_active_{}, helper_joined_{}, helper_closed_{}, helper_verified_{},
        helper_absent_{};
    std::unique_ptr<LoopState> loop_;
    std::array<Q2ControlledFailure, Q2ControlledFailure::maximum_cleanup + 2U>
        compact_failure_stock_{};
    std::size_t compact_failure_stock_index_{};
    friend struct MonitoringQ2ControlledEventSessionTestAccess;
};
struct MonitoringQ2ControlledEventSessionTestAccess {
    using PublicationHook = void (*)(void*) noexcept;
    using HelperWaitHook = void (*)(void*, std::uint32_t) noexcept;
    static void install_publication_hook(PublicationHook) noexcept;
    static void install_helper_wait_hook(HelperWaitHook) noexcept;
    static void pump_helper(void*) noexcept;
    static void run_helper_once(void*) noexcept;
};
} // namespace epycbench::monitoring::qualification::detail
