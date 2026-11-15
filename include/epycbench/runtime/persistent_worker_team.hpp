#pragma once

#include "epycbench/runtime/admitted_placement_request.hpp"
#include "epycbench/runtime/worker_execution.hpp"

#include <expected>
#include <memory>
#include <span>

namespace epycbench::runtime {

namespace detail {
struct PersistentWorkerTeamAccess;
struct PersistentWorkerGenerationState;
class TargetedWorkerGeneration;
} // namespace detail

class PersistentWorkerGeneration;

class PersistentWorkerTeam {
  public:
    [[nodiscard]] static auto create(std::span<const AdmittedWorker> workers,
                                     RunnerCancellation& cancellation) noexcept
        -> std::expected<PersistentWorkerTeam, core::Error>;

    [[nodiscard]] auto readiness() const noexcept -> std::span<const WorkerReadyEvidence>;
    [[nodiscard]] auto
    execute(WorkerOperation operation, WorkerObservationPolicy observation,
            WorkerCancellationPolicy cancellation_policy = WorkerCancellationPolicy::observe)
        -> std::expected<WorkerTeamCommandOutcome, core::Error>;
    [[nodiscard]] auto begin_generation(
        WorkerOperation operation, WorkerObservationPolicy observation,
        WorkerCancellationPolicy cancellation_policy = WorkerCancellationPolicy::observe)
        -> std::expected<PersistentWorkerGeneration, core::Error>;
    [[nodiscard]] auto stop_and_join() -> std::expected<void, core::Error>;

    [[nodiscard]] auto ready() const noexcept -> bool;
    [[nodiscard]] auto active() const noexcept -> bool;

    PersistentWorkerTeam(PersistentWorkerTeam&&) noexcept;
    PersistentWorkerTeam(const PersistentWorkerTeam&) = delete;
    auto operator=(const PersistentWorkerTeam&) -> PersistentWorkerTeam& = delete;
    auto operator=(PersistentWorkerTeam&&) -> PersistentWorkerTeam& = delete;
    ~PersistentWorkerTeam() noexcept;

  private:
    struct State;
    explicit PersistentWorkerTeam(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend struct detail::PersistentWorkerTeamAccess;
    friend class detail::TargetedWorkerGeneration;
    friend class PersistentWorkerGeneration;
};

class PersistentWorkerGeneration {
  public:
    [[nodiscard]] auto await_ready() -> std::expected<void, core::Error>;
    void cancel_before_release() noexcept;
    void release() noexcept;
    [[nodiscard]] auto await_timed_completion() -> std::expected<void, core::Error>;
    [[nodiscard]] auto await_quiescence() -> std::expected<void, core::Error>;
    [[nodiscard]] auto collect_ordered_results()
        -> std::expected<WorkerTeamCommandOutcome, core::Error>;

    PersistentWorkerGeneration(PersistentWorkerGeneration&&) noexcept;
    PersistentWorkerGeneration(const PersistentWorkerGeneration&) = delete;
    auto operator=(const PersistentWorkerGeneration&) -> PersistentWorkerGeneration& = delete;
    auto operator=(PersistentWorkerGeneration&&) -> PersistentWorkerGeneration& = delete;
    ~PersistentWorkerGeneration() noexcept;

  private:
    PersistentWorkerGeneration(
        void* team_state, std::unique_ptr<detail::PersistentWorkerGenerationState> state) noexcept;

    void* team_state_{};
    std::unique_ptr<detail::PersistentWorkerGenerationState> state_;

    friend class PersistentWorkerTeam;
    friend struct detail::PersistentWorkerTeamAccess;
};

} // namespace epycbench::runtime
