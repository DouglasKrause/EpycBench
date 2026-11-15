#pragma once

#include "monitoring/host_q2_controlled_event_session.hpp"
#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"
#include <variant>

namespace epycbench::monitoring::qualification::detail {
auto run_q2_controlled_target_body(const Q2TargetSnapshot&, std::size_t, std::atomic<bool>&,
                                   std::atomic<bool>&, const runtime::WorkerExecutionContext&,
                                   const runtime::detail::TargetedWorkerExecutionControl&) noexcept
    -> runtime::detail::TargetedWorkerCallbackResult;
void run_q2_target_event_loop(Q2ControlledGenerationProtocol&, std::uint64_t,
                              std::atomic<std::uint64_t>&, std::atomic<std::uint64_t>&,
                              const runtime::detail::TargetedWorkerExecutionControl&) noexcept;
auto run_q2_voluntary_readiness_body(const Q2TargetSnapshot&, std::size_t,
                                     Q2ControlledGenerationProtocol&, std::uint64_t,
                                     const runtime::WorkerExecutionContext&) noexcept -> bool;
auto run_q2_voluntary_loop_body(Q2ControlledGenerationProtocol&, std::uint64_t,
                                const runtime::detail::TargetedWorkerExecutionControl&,
                                MonitoringQ2DeadlineCancellationContext) noexcept -> bool;

class MonitoringQ2BridgeRetirementRecovery;
class MonitoringQ2QuiescedWorkerBorrow final {
  public:
    MonitoringQ2QuiescedWorkerBorrow(MonitoringQ2QuiescedWorkerBorrow&&) noexcept;
    MonitoringQ2QuiescedWorkerBorrow(const MonitoringQ2QuiescedWorkerBorrow&) = delete;
    ~MonitoringQ2QuiescedWorkerBorrow() noexcept;

  private:
    MonitoringQ2QuiescedWorkerBorrow(MonitoringQ2WorkerSession* owner, std::size_t index,
                                     Q2TargetSnapshot expected, Q2TargetOperations operations,
                                     std::shared_ptr<void> lifetime) noexcept
        : owner_{owner}, index_{index}, expected_{std::move(expected)}, operations_{operations},
          lifetime_{std::move(lifetime)} {}
    MonitoringQ2WorkerSession* owner_{};
    std::size_t index_{};
    Q2TargetSnapshot expected_;
    Q2TargetOperations operations_{};
    std::shared_ptr<void> lifetime_;
    bool consumed_{};
    friend class MonitoringQ2ControlledEventRuntimeBridge;
    friend class MonitoringQ2BridgeRetirementRecovery;
    friend class MonitoringQ2WorkerSession;
    friend class MonitoringQ2AuthoritySessionTestAccess;
};

struct MonitoringQ2RecoveredWorkerFailure final {
    MonitoringQ2QuiescedWorkerBorrow borrow;
    Q2ControlledFailure failure;
};

class MonitoringQ2BridgeRetirementRecovery final {
  public:
    MonitoringQ2BridgeRetirementRecovery(MonitoringQ2BridgeRetirementRecovery&&) noexcept;
    MonitoringQ2BridgeRetirementRecovery(const MonitoringQ2BridgeRetirementRecovery&) = delete;
    ~MonitoringQ2BridgeRetirementRecovery() noexcept;
    auto retry(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::optional<MonitoringQ2RecoveredWorkerFailure>;
    auto failure() const noexcept -> const Q2ControlledFailure&;

  private:
    auto take_failure_for_containment() noexcept -> Q2ControlledFailure;
    struct Impl;
    explicit MonitoringQ2BridgeRetirementRecovery(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class MonitoringQ2ControlledEventRuntimeBridge;
    friend class MonitoringQ2TerminalContainment;
    friend class MonitoringQ2AuthoritySessionTestAccess;
};
using MonitoringQ2BridgeRetirementOutcome =
    std::variant<MonitoringQ2QuiescedWorkerBorrow, MonitoringQ2BridgeRetirementRecovery,
                 MonitoringQ2RecoveredWorkerFailure>;

class MonitoringQ2ControlledEventRuntimeBridge final {
  public:
    struct State;
    static auto create_injected(std::size_t, Q2TargetSnapshot,
                                Q2RuntimeGenerationOperations) noexcept
        -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error>;
    MonitoringQ2ControlledEventRuntimeBridge(MonitoringQ2ControlledEventRuntimeBridge&&) noexcept;
    MonitoringQ2ControlledEventRuntimeBridge(const MonitoringQ2ControlledEventRuntimeBridge&) =
        delete;
    ~MonitoringQ2ControlledEventRuntimeBridge() noexcept;

    auto begin(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2LifecycleResult;
    auto await_ready(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2LifecycleResult;
    auto release(std::uint64_t) noexcept -> Q2LifecycleResult;
    auto drain_terminal(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2LifecycleResult;
    auto repark(std::uint64_t, MonitoringQ2DeadlineCancellationContext) noexcept
        -> Q2LifecycleResult;
    auto retire(MonitoringQ2ControlledEventSession&&,
                MonitoringQ2DeadlineCancellationContext) noexcept
        -> MonitoringQ2BridgeRetirementOutcome;
    auto snapshot() const noexcept -> const Q2TargetSnapshot*;
    auto target_operations() noexcept -> Q2TargetOperations;
    void set_iteration_for_test(std::uint64_t) noexcept;

  private:
    auto retire_before_controlled_session() noexcept -> MonitoringQ2QuiescedWorkerBorrow;
    static auto create(MonitoringQ2WorkerSession&, std::size_t,
                       const MonitoringQ2WorkerBinding&) noexcept
        -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error>;
    static auto adopt_existing_borrow(MonitoringQ2SourcePreparationWorkerHandoff&&,
                                      void* test_context = nullptr,
                                      void (*before_allocation_test)(void*) = nullptr) noexcept
        -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error>;
    static auto create_borrow_injected(MonitoringQ2WorkerSession&, std::size_t, Q2TargetSnapshot,
                                       Q2RuntimeGenerationOperations) noexcept
        -> std::expected<MonitoringQ2ControlledEventRuntimeBridge, core::Error>;
    explicit MonitoringQ2ControlledEventRuntimeBridge(std::shared_ptr<State>) noexcept;
    std::shared_ptr<State> state_;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2ContextSwitchSourceOwner;
    friend class MonitoringQ2ContextSwitchCollector;
};

} // namespace epycbench::monitoring::qualification::detail
