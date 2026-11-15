#pragma once

#include "monitoring/host_q2_native_thread_owner.hpp"
#include "runtime/monitoring_q1_targeted_generation_bridge.hpp"

namespace epycbench::monitoring::qualification::detail {

struct Q2RuntimeGenerationOperations;
class MonitoringQ2NativeWorkerTeamCreateOutcome;
class MonitoringQ2NativeWorkerTeamTestAccess;

class MonitoringQ2NativeWorkerTeam final {
  public:
    struct CreateFailure final {
        explicit CreateFailure(MonitoringQ2CompactDiagnostic value) noexcept : primary{value} {}
        MonitoringQ2CompactDiagnostic primary;
        MonitoringQ2CompactLedger<512> cleanup;
        std::optional<core::Error> authored_primary;
    };
    static auto create(runtime::detail::MonitoringQ1AdmittedWorkerSnapshot&&,
                       runtime::detail::WorkerTeamDependencies,
                       MonitoringQ2DeadlineCancellationContext,
                       const MonitoringQ2NativeThreadSystem* = nullptr) noexcept
        -> MonitoringQ2NativeWorkerTeamCreateOutcome;
    MonitoringQ2NativeWorkerTeam(MonitoringQ2NativeWorkerTeam&&) noexcept;
    MonitoringQ2NativeWorkerTeam(const MonitoringQ2NativeWorkerTeam&) = delete;
    ~MonitoringQ2NativeWorkerTeam() noexcept;
    auto readiness(std::size_t) const noexcept
        -> std::expected<runtime::WorkerReadyEvidence, core::Error>;
    auto generation_operations() noexcept -> Q2RuntimeGenerationOperations;
    auto retire(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, CreateFailure>;
    auto active() const noexcept -> bool;
    // Controller-only borrow, serialized with begin/reset/retire. Invalid or
    // inactive generation state is a refusal, not a fresh cancellation source.
    auto cancellation_requested(std::size_t index) const noexcept -> bool;

  private:
    struct State;
    explicit MonitoringQ2NativeWorkerTeam(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend class MonitoringQ2NativeWorkerTeamCreateOutcome;
    friend class MonitoringQ2NativeWorkerTeamTestAccess;
};

class MonitoringQ2NativeWorkerTeamCreateOutcome final {
  public:
    MonitoringQ2NativeWorkerTeamCreateOutcome(
        MonitoringQ2NativeWorkerTeamCreateOutcome&&) noexcept = default;
    MonitoringQ2NativeWorkerTeamCreateOutcome(const MonitoringQ2NativeWorkerTeamCreateOutcome&) =
        delete;
    explicit operator bool() const noexcept { return team_.has_value() && !failure_; }
    auto team() && noexcept -> MonitoringQ2NativeWorkerTeam { return std::move(*team_); }
    auto error() const noexcept -> const MonitoringQ2NativeWorkerTeam::CreateFailure& {
        return *failure_;
    }
    auto take_failure() && noexcept -> MonitoringQ2NativeWorkerTeam::CreateFailure {
        return std::move(*failure_);
    }
    auto recovery_team() && noexcept -> std::optional<MonitoringQ2NativeWorkerTeam> {
        return std::move(team_);
    }

  private:
    MonitoringQ2NativeWorkerTeamCreateOutcome(
        std::optional<MonitoringQ2NativeWorkerTeam> team,
        std::optional<MonitoringQ2NativeWorkerTeam::CreateFailure> failure) noexcept
        : team_{std::move(team)}, failure_{std::move(failure)} {}
    std::optional<MonitoringQ2NativeWorkerTeam> team_;
    std::optional<MonitoringQ2NativeWorkerTeam::CreateFailure> failure_;
    friend class MonitoringQ2NativeWorkerTeam;
};

} // namespace epycbench::monitoring::qualification::detail
