#pragma once

#include "epycbench/runtime/runner_control_monitor.hpp"
#include "epycbench/runtime/worker_execution.hpp"
#include "epycbench/measurement/repetition.hpp"
#include "epycbench/measurement/warmup.hpp"
#include "runtime/pairwise_platform_leaf_facade.hpp"
#include "runtime/worker_team_state.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace epycbench::runtime::detail {

struct PairwiseWorkerReadinessBorrowState;
class PairwisePreparedMonitoringStorage;

// A source-private, one-shot borrow of the authenticated readiness records
// retained by a still-live Pairwise worker team.  The capability deliberately
// has no public record/TID projection.
class PairwiseWorkerReadinessCapability final {
  public:
    PairwiseWorkerReadinessCapability(
        const PairwiseWorkerReadinessCapability&) = delete;
    PairwiseWorkerReadinessCapability(
        PairwiseWorkerReadinessCapability&&) noexcept;
    auto operator=(const PairwiseWorkerReadinessCapability&)
        -> PairwiseWorkerReadinessCapability& = delete;
    auto operator=(PairwiseWorkerReadinessCapability&&) noexcept
        -> PairwiseWorkerReadinessCapability&;
    ~PairwiseWorkerReadinessCapability() noexcept;

  private:
    PairwiseWorkerReadinessCapability(
        std::weak_ptr<PairwiseWorkerReadinessBorrowState> borrow,
        std::uint64_t generation) noexcept
        : borrow_{std::move(borrow)}, generation_{generation} {}
    [[nodiscard]] auto consume() noexcept
        -> std::pair<std::span<const WorkerLaunchSpec>,
                     std::span<const WorkerReadyEvidence>>;
    void release() noexcept;
    std::weak_ptr<PairwiseWorkerReadinessBorrowState> borrow_;
    std::uint64_t generation_{};
    bool consumed_{};
    friend class PairwisePersistentWorkerTeam;
    friend class PairwisePreparedMonitoringStorage;
};

class PairwiseWorkerOperationFailure final {
  public:
    [[nodiscard]] static auto create(PairwiseEvidenceFailure failure) noexcept
        -> std::optional<PairwiseWorkerOperationFailure>;
    [[nodiscard]] auto failure() const noexcept -> const PairwiseEvidenceFailure& {
        return failure_;
    }

  private:
    explicit PairwiseWorkerOperationFailure(PairwiseEvidenceFailure failure) noexcept
        : failure_{std::move(failure)} {}
    PairwiseEvidenceFailure failure_;
};

struct PairwiseWorkerCallbackFailure final {
    PairwiseWorkerOperationFailure primary;
    std::optional<PairwiseWorkerOperationFailure> cleanup;
};

struct PairwiseWorkerExecutionControl final {
    RunnerCancellation& runner_cancellation;
    std::stop_token team_stop;
    std::stop_token command_stop;
    void* signal_context{};
    void (*signal)(void*) noexcept{};

    void signal_timed_completion() const noexcept {
        if (signal != nullptr)
            signal(signal_context);
    }
};

using PairwiseWorkerCallbackResult = std::expected<void, PairwiseWorkerCallbackFailure>;
struct PairwiseWorkerOperation final {
    void* context{};
    PairwiseWorkerCallbackResult (*run)(void*, const WorkerExecutionContext&,
                                        const PairwiseWorkerExecutionControl&){};
};

struct PairwiseWorkerCommandResult final {
    core::WorkerId worker_id;
    std::optional<PairwiseWorkerFailure> failure;
    std::optional<PairwiseEvidenceFailure> operation_primary;
    std::optional<PairwiseEvidenceFailure> operation_cleanup;
    std::optional<core::LinuxCpuId> cpu_before;
    std::optional<core::LinuxCpuId> cpu_after;
    bool migrated{};
    bool cancellation_observed{};
    bool peer_stopped{};
};

class PairwiseWorkerCommandOutcome final {
  public:
    struct State;
    PairwiseWorkerCommandOutcome(PairwiseWorkerCommandOutcome&&) noexcept;
    PairwiseWorkerCommandOutcome(const PairwiseWorkerCommandOutcome&) = delete;
    auto operator=(const PairwiseWorkerCommandOutcome&)
        -> PairwiseWorkerCommandOutcome& = delete;
    auto operator=(PairwiseWorkerCommandOutcome&&) noexcept
        -> PairwiseWorkerCommandOutcome&;
    ~PairwiseWorkerCommandOutcome() noexcept;

    [[nodiscard]] auto workers() const noexcept -> std::span<const PairwiseWorkerCommandResult>;
    [[nodiscard]] auto cancelled() const noexcept -> bool;

  private:
    explicit PairwiseWorkerCommandOutcome(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
    friend class PairwisePersistentWorkerGeneration;
};

enum class PairwiseWorkerGenerationClass : std::uint8_t {
    memory_preparation,
    calibration,
    warmup,
    observation,
    finalization_cleanup,
};

class PairwisePreparedWorkerTeam final {
  public:
    struct State;
    PairwisePreparedWorkerTeam(PairwisePreparedWorkerTeam&&) noexcept;
    PairwisePreparedWorkerTeam(const PairwisePreparedWorkerTeam&) = delete;
    auto operator=(const PairwisePreparedWorkerTeam&)
        -> PairwisePreparedWorkerTeam& = delete;
    auto operator=(PairwisePreparedWorkerTeam&&) noexcept
        -> PairwisePreparedWorkerTeam&;
    ~PairwisePreparedWorkerTeam() noexcept;

    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;
    [[nodiscard]] auto generation_capacity() const noexcept -> std::size_t;

  private:
    explicit PairwisePreparedWorkerTeam(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
    friend class PairwiseWorkerOwningFacade;
};

struct PairwiseWorkerFailureSequence final {
    PairwiseWorkerFailure primary;
    std::vector<PairwiseWorkerFailure> cleanup;
};

enum class PairwiseWorkerOwningOperation : std::uint8_t {
    prepare,
    launch,
    startup,
    execute,
    generation,
    callback,
    result_publication,
    projection,
    join,
};
enum class PairwiseTypedClockSite : std::uint8_t {
    empty_start, empty_end, controller_start, controller_end,
    worker_start, worker_end,
};
enum class PairwiseTypedObservationFault : std::uint8_t {
    none, interval, worker_validation, observation, validity,
};

struct PairwiseWorkerOwningHooks final {
    void* context{};
    void (*before_operation)(void*, PairwiseWorkerOwningOperation, std::size_t){};
    void (*before_join)(void*, std::size_t){};
    void (*adjust_team_state)(void*, std::uint8_t&) noexcept{};
    void (*adjust_generation)(void*, std::uint64_t&) noexcept{};
    void (*before_clock)(void*, PairwiseTypedClockSite, bool,
                         std::optional<core::WorkerId>) noexcept{};
    PairwiseTypedObservationFault observation_fault{
        PairwiseTypedObservationFault::none};
};

struct PairwiseWorkerThreadLauncher final {
    void* context{};
    std::jthread (*launch)(void*, void (*)(void*, std::size_t) noexcept,
                           void*, std::size_t){};
};

struct PairwiseWorkerOwningDependencies final {
    PairwisePlatformLeafFacade platform;
    PairwiseWorkerThreadLauncher launcher;
    PairwiseWorkerOwningHooks hooks{};
};

class PairwisePersistentWorkerGeneration;

class PairwisePersistentWorkerTeam final {
  public:
    struct State;
    PairwisePersistentWorkerTeam(PairwisePersistentWorkerTeam&&) noexcept;
    PairwisePersistentWorkerTeam(const PairwisePersistentWorkerTeam&) = delete;
    auto operator=(const PairwisePersistentWorkerTeam&)
        -> PairwisePersistentWorkerTeam& = delete;
    auto operator=(PairwisePersistentWorkerTeam&&) noexcept
        -> PairwisePersistentWorkerTeam&;
    ~PairwisePersistentWorkerTeam() noexcept;

    [[nodiscard]] auto readiness() const noexcept -> std::span<const WorkerReadyEvidence>;
    [[nodiscard]] auto lend_monitoring_readiness() & noexcept
        -> std::optional<PairwiseWorkerReadinessCapability>;
    [[nodiscard]] auto begin_generation(
        PairwiseWorkerGenerationClass generation_class,
        PairwiseWorkerOperation operation,
        WorkerObservationPolicy observation,
        WorkerCancellationPolicy cancellation_policy = WorkerCancellationPolicy::observe) noexcept
        -> std::expected<PairwisePersistentWorkerGeneration, PairwiseWorkerFailure>;
    [[nodiscard]] auto execute(
        PairwiseWorkerGenerationClass generation_class,
        PairwiseWorkerOperation operation,
        WorkerObservationPolicy observation,
        WorkerCancellationPolicy cancellation_policy = WorkerCancellationPolicy::observe) noexcept
        -> std::expected<PairwiseWorkerCommandOutcome, PairwiseWorkerFailure>;
    [[nodiscard]] auto stop_and_join() noexcept
        -> std::expected<void, PairwiseWorkerFailureSequence>;
    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto active() const noexcept -> bool;

  private:
    [[nodiscard]] auto progress_quiescent() const noexcept -> bool;
    friend class PairwiseTypedOwnerTransaction;
    explicit PairwisePersistentWorkerTeam(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
    friend class PairwiseWorkerOwningFacade;
    friend class PairwisePersistentWorkerGeneration;
    friend class PairwiseWorkerOwningTestAccess;
};

class PairwisePersistentWorkerGeneration final {
  public:
    PairwisePersistentWorkerGeneration(PairwisePersistentWorkerGeneration&&) noexcept;
    PairwisePersistentWorkerGeneration(const PairwisePersistentWorkerGeneration&) = delete;
    auto operator=(const PairwisePersistentWorkerGeneration&)
        -> PairwisePersistentWorkerGeneration& = delete;
    auto operator=(PairwisePersistentWorkerGeneration&&) noexcept
        -> PairwisePersistentWorkerGeneration&;
    ~PairwisePersistentWorkerGeneration() noexcept;

    [[nodiscard]] auto await_ready() noexcept
        -> std::expected<void, PairwiseWorkerFailure>;
    void cancel_before_release() noexcept;
    void release() noexcept;
    [[nodiscard]] auto await_timed_completion() noexcept
        -> std::expected<void, PairwiseWorkerFailure>;
    [[nodiscard]] auto await_quiescence() noexcept
        -> std::expected<void, PairwiseWorkerFailure>;
    [[nodiscard]] auto collect_ordered_results() noexcept
        -> std::expected<PairwiseWorkerCommandOutcome, PairwiseWorkerFailure>;

  private:
    PairwisePersistentWorkerGeneration(
        void* team, std::unique_ptr<PairwiseWorkerCommandOutcome::State> state) noexcept;
    void* team_{};
    std::unique_ptr<PairwiseWorkerCommandOutcome::State> state_;
    bool collected_{};
    friend class PairwisePersistentWorkerTeam;
    friend class PairwiseWorkerOwningTestAccess;
};

// Source-private access to the real signal edge.  It exists only so the
// invariant branches at that edge can be proved without exposing them through
// the public Runtime API.
class PairwiseWorkerOwningTestAccess final {
  public:
    static void signal(PairwisePersistentWorkerGeneration& generation,
                       std::size_t worker) noexcept;
    [[nodiscard]] static auto author_failure(
        PairwiseFailureCode code, PairwiseFailureOperation operation,
        PairwiseFailureReason reason, PairwiseFailureContext context,
        std::optional<int> error_number = std::nullopt) noexcept
        -> PairwiseWorkerFailure;
    [[nodiscard]] static auto forced_errors(
        const PairwisePersistentWorkerTeam& team) noexcept
        -> std::span<const ForcedTeamStopError>;
};

class PairwiseWorkerOwningFacade final {
  public:
    [[nodiscard]] static auto production_dependencies() noexcept
        -> PairwiseWorkerOwningDependencies;
    [[nodiscard]] static auto prepare(
        std::span<const WorkerLaunchSpec> workers,
        RunnerCancellation& cancellation,
        const measurement::WarmupPolicy& warmup,
        const measurement::RepetitionPolicy& repetition,
        PairwisePlatformLeafFacade platform) noexcept
        -> std::expected<PairwisePreparedWorkerTeam, PairwiseEvidenceFailure>;
    [[nodiscard]] static auto start(
        PairwisePreparedWorkerTeam prepared,
        PairwiseWorkerOwningDependencies dependencies) noexcept
        -> std::expected<PairwisePersistentWorkerTeam, PairwiseWorkerFailureSequence>;
};

} // namespace epycbench::runtime::detail
