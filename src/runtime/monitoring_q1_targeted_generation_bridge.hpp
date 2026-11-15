#pragma once

#include "runtime/worker_team_state.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2AuthoritySessionTestAccess;
class MonitoringQ2NativeWorkerTeam;
}

namespace epycbench::runtime::detail {

enum class MonitoringQ1WorkerSessionDisposition : std::uint8_t {
    succeeded, failed, active_generation, session_moved
};
class MonitoringQ1WorkerSession;
struct MonitoringQ1WorkerSessionTestAccess;
class MonitoringQ1AdmittedWorkerSnapshot final {
  public:
    MonitoringQ1AdmittedWorkerSnapshot(MonitoringQ1AdmittedWorkerSnapshot&&) noexcept = default;
    MonitoringQ1AdmittedWorkerSnapshot(const MonitoringQ1AdmittedWorkerSnapshot&) = delete;
    auto operator=(MonitoringQ1AdmittedWorkerSnapshot&&)
        -> MonitoringQ1AdmittedWorkerSnapshot& = delete;
    auto operator=(const MonitoringQ1AdmittedWorkerSnapshot&)
        -> MonitoringQ1AdmittedWorkerSnapshot& = delete;
    ~MonitoringQ1AdmittedWorkerSnapshot() noexcept = default;
  private:
    explicit MonitoringQ1AdmittedWorkerSnapshot(std::vector<WorkerLaunchSpec> records) noexcept
        : records_{std::move(records)} {}
    std::vector<WorkerLaunchSpec> records_;
    friend auto project_monitoring_q1_worker_snapshot(const AdmittedPlacementRequest&) noexcept
      -> std::expected<MonitoringQ1AdmittedWorkerSnapshot, core::Error>;
    friend class MonitoringQ1WorkerSession;
    friend auto make_monitoring_q1_worker_session(MonitoringQ1AdmittedWorkerSnapshot&&,
      RunnerCancellation&,WorkerTeamDependencies)
      ->std::expected<MonitoringQ1WorkerSession,core::Error>;
    friend auto begin_monitoring_q1_targeted_generation(MonitoringQ1WorkerSession&,
      std::size_t,TargetedWorkerOperation,WorkerCancellationPolicy)
      ->std::expected<TargetedWorkerGeneration,core::Error>;
    friend auto begin_monitoring_q2_targeted_generation(MonitoringQ1WorkerSession&,
      std::size_t,TargetedWorkerOperation,TargetedWorkerReadinessOperation,
      WorkerCancellationPolicy)->std::expected<TargetedWorkerGeneration,core::Error>;
    friend auto monitoring_q2_worker_readiness(const MonitoringQ1WorkerSession&,std::size_t) noexcept
      ->std::expected<WorkerReadyEvidence,core::Error>;
    friend class epycbench::monitoring::qualification::detail::MonitoringQ2AuthoritySessionTestAccess;
    friend class epycbench::monitoring::qualification::detail::MonitoringQ2NativeWorkerTeam;
};

class MonitoringQ1WorkerSessionFinalization final {
  public:
    MonitoringQ1WorkerSessionFinalization(MonitoringQ1WorkerSessionFinalization&&) noexcept = default;
    MonitoringQ1WorkerSessionFinalization(const MonitoringQ1WorkerSessionFinalization&) = delete;
    auto operator=(MonitoringQ1WorkerSessionFinalization&&)
        -> MonitoringQ1WorkerSessionFinalization& = delete;
    auto operator=(const MonitoringQ1WorkerSessionFinalization&)
        -> MonitoringQ1WorkerSessionFinalization& = delete;
    ~MonitoringQ1WorkerSessionFinalization() noexcept = default;
    [[nodiscard]] auto disposition() const noexcept { return disposition_; }
    [[nodiscard]] auto primary() const noexcept -> const std::optional<core::Error>& { return primary_; }
    [[nodiscard]] auto secondary() const noexcept -> std::span<const core::Error> { return secondary_; }
    [[nodiscard]] auto take_primary() && noexcept -> std::optional<core::Error> {
        return std::move(primary_);
    }
    [[nodiscard]] auto take_secondary() && noexcept -> std::vector<core::Error> {
        return std::move(secondary_);
    }
  private:
    MonitoringQ1WorkerSessionDisposition disposition_;
    std::optional<core::Error> primary_;
    std::vector<core::Error> secondary_;
    MonitoringQ1WorkerSessionFinalization(MonitoringQ1WorkerSessionDisposition,
      std::optional<core::Error>,std::vector<core::Error>) noexcept;
    friend class MonitoringQ1WorkerSession;
    friend auto finalize_monitoring_q1_worker_session(MonitoringQ1WorkerSession&&) noexcept
      ->MonitoringQ1WorkerSessionFinalization;
    friend struct MonitoringQ1WorkerSessionTestAccess;
};

class MonitoringQ1WorkerSession final {
  public:
    MonitoringQ1WorkerSession(MonitoringQ1WorkerSession&&) noexcept;
    MonitoringQ1WorkerSession(const MonitoringQ1WorkerSession&)=delete;
    auto operator=(const MonitoringQ1WorkerSession&) -> MonitoringQ1WorkerSession& = delete;
    auto operator=(MonitoringQ1WorkerSession&&) -> MonitoringQ1WorkerSession& = delete;
    ~MonitoringQ1WorkerSession() noexcept;
  private:
    struct State;
    explicit MonitoringQ1WorkerSession(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend auto create_monitoring_q1_worker_session(const AdmittedPlacementRequest&,
      RunnerCancellation&)->std::expected<MonitoringQ1WorkerSession,core::Error>;
    friend auto make_monitoring_q1_worker_session(
      MonitoringQ1AdmittedWorkerSnapshot&&,RunnerCancellation&,WorkerTeamDependencies)
      ->std::expected<MonitoringQ1WorkerSession,core::Error>;
    friend auto begin_monitoring_q1_targeted_generation(MonitoringQ1WorkerSession&,std::size_t,
      TargetedWorkerOperation,WorkerCancellationPolicy)->std::expected<TargetedWorkerGeneration,core::Error>;
    friend auto begin_monitoring_q2_targeted_generation(MonitoringQ1WorkerSession&,std::size_t,
      TargetedWorkerOperation,TargetedWorkerReadinessOperation,WorkerCancellationPolicy)
      ->std::expected<TargetedWorkerGeneration,core::Error>;
    friend auto monitoring_q2_worker_readiness(const MonitoringQ1WorkerSession&,std::size_t) noexcept
      ->std::expected<WorkerReadyEvidence,core::Error>;
    friend auto reset_monitoring_q1_targeted_control(MonitoringQ1WorkerSession&) noexcept
      ->std::expected<void,core::Error>;
    friend auto finalize_monitoring_q1_worker_session(MonitoringQ1WorkerSession&&) noexcept
      ->MonitoringQ1WorkerSessionFinalization;
    friend struct MonitoringQ1WorkerSessionTestAccess;
};

[[nodiscard]] auto project_monitoring_q1_worker_snapshot(
    const AdmittedPlacementRequest&) noexcept
    -> std::expected<MonitoringQ1AdmittedWorkerSnapshot, core::Error>;

[[nodiscard]] auto create_monitoring_q1_worker_session(
    const AdmittedPlacementRequest&,RunnerCancellation&)
    ->std::expected<MonitoringQ1WorkerSession,core::Error>;
[[nodiscard]] auto make_monitoring_q1_worker_session(
    MonitoringQ1AdmittedWorkerSnapshot&&, RunnerCancellation&, WorkerTeamDependencies)
    -> std::expected<MonitoringQ1WorkerSession, core::Error>;
[[nodiscard]] auto make_monitoring_q2_worker_session(
    MonitoringQ1AdmittedWorkerSnapshot&&, RunnerCancellation&)
    -> std::expected<MonitoringQ1WorkerSession, core::Error>;
[[nodiscard]] auto monitoring_q2_worker_readiness(
    const MonitoringQ1WorkerSession&,std::size_t) noexcept
    ->std::expected<WorkerReadyEvidence,core::Error>;
[[nodiscard]] auto begin_monitoring_q1_targeted_generation(
    MonitoringQ1WorkerSession&,std::size_t,TargetedWorkerOperation,
    WorkerCancellationPolicy=WorkerCancellationPolicy::observe)
    ->std::expected<TargetedWorkerGeneration,core::Error>;
[[nodiscard]] auto begin_monitoring_q2_targeted_generation(
    MonitoringQ1WorkerSession&,std::size_t,TargetedWorkerOperation,
    TargetedWorkerReadinessOperation,
    WorkerCancellationPolicy=WorkerCancellationPolicy::observe)
    ->std::expected<TargetedWorkerGeneration,core::Error>;
[[nodiscard]] auto begin_monitoring_q2_targeted_generation(
    const AdmittedPlacementRequest&,PersistentWorkerTeam&,TargetedGenerationControl&,
    std::size_t,TargetedWorkerOperation,TargetedWorkerReadinessOperation,
    WorkerCancellationPolicy=WorkerCancellationPolicy::observe)
    ->std::expected<TargetedWorkerGeneration,core::Error>;
[[nodiscard]] auto reset_monitoring_q1_targeted_control(MonitoringQ1WorkerSession&) noexcept
    ->std::expected<void,core::Error>;
[[nodiscard]] auto finalize_monitoring_q1_worker_session(MonitoringQ1WorkerSession&&) noexcept
    ->MonitoringQ1WorkerSessionFinalization;

[[nodiscard]] auto make_monitoring_q1_targeted_control()
    -> std::expected<TargetedGenerationControl, core::Error>;

[[nodiscard]] auto begin_monitoring_q1_targeted_generation(
    const AdmittedPlacementRequest&, PersistentWorkerTeam&, TargetedGenerationControl&,
    std::size_t target_index, TargetedWorkerOperation,
    WorkerCancellationPolicy = WorkerCancellationPolicy::observe)
    -> std::expected<TargetedWorkerGeneration, core::Error>;

[[nodiscard]] auto reset_monitoring_q1_targeted_control(TargetedGenerationControl&) noexcept
    -> std::expected<void, core::Error>;

} // namespace epycbench::runtime::detail
