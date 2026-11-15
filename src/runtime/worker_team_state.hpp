#pragma once

#include "epycbench/runtime/persistent_worker_team.hpp"
#include "runtime/worker_platform_operations.hpp"
#include "runtime/worker_thread_launcher.hpp"

#include <expected>
#include <exception>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace epycbench::runtime::detail {

struct WorkerLaunchSpec {
    std::size_t worker_index;
    core::WorkerId worker_id;
    core::ProcessingUnitId processing_unit;
    core::CoreId core;
    topology::StableProcessingUnitId stable_pu;
    topology::StableCoreId stable_core;
    core::LinuxCpuId requested_cpu;
};

struct WorkerTeamDependencies {
    WorkerPlatformOperations platform;
    WorkerThreadLauncher launcher;
};

enum class ForcedTeamStopDisposition : std::uint8_t { inactive, active };
enum class ForcedTeamStopErrorKind : std::uint8_t {
    join_failed, contained_exception, contained_unknown_failure, capacity_exhausted
};
struct ForcedTeamStopError {
    ForcedTeamStopErrorKind kind{};
    std::uint8_t sweep{};
    std::uint32_t worker_index{};
};
struct ForcedTeamStopResult {
    ForcedTeamStopDisposition disposition{};
    std::uint8_t sweeps_attempted{};
    std::span<const ForcedTeamStopError> errors{};
    bool error_capacity_exhausted{};
};
enum class CleanupWorkerDisposition : std::uint8_t {
    pristine, succeeded, primary_failure, contained_exception, contained_unknown
};
struct CleanupGenerationTag;
using CleanupGeneration = core::StrongId<CleanupGenerationTag, std::uint64_t>;
struct CleanupWorkerFailure {
    core::Error primary;
    std::optional<core::Error> cleanup;
};
using CleanupWorkerCallbackResult = std::expected<void, CleanupWorkerFailure>;
struct CleanupWorkerOperation {
    void* context{};
    CleanupWorkerCallbackResult (*run)(void*, const WorkerExecutionContext&,
                                       const WorkerExecutionControl&){};
};
struct CleanupWorkerSlot {
    explicit CleanupWorkerSlot(core::WorkerId id) noexcept : worker_id{id} {}
    CleanupWorkerSlot(const CleanupWorkerSlot&) = delete;
    auto operator=(const CleanupWorkerSlot&) -> CleanupWorkerSlot& = delete;
    CleanupWorkerSlot(CleanupWorkerSlot&& other) noexcept : worker_id{other.worker_id} {
        if (other.disposition != CleanupWorkerDisposition::pristine || other.primary ||
            other.cleanup || other.owner_state_cookie_ != nullptr ||
            other.self_address_cookie_ != nullptr || other.cleanup_generation_ != 0)
            std::terminate();
    }
    auto operator=(CleanupWorkerSlot&&) -> CleanupWorkerSlot& = delete;
    core::WorkerId worker_id;
    CleanupWorkerDisposition disposition{CleanupWorkerDisposition::pristine};
    std::optional<core::Error> primary;
    std::optional<core::Error> cleanup;
  private:
    const void* owner_state_cookie_{};
    const void* self_address_cookie_{};
    std::uint64_t cleanup_generation_{};
    friend struct PersistentWorkerTeamAccess;
};
struct CleanupCommandResult { CleanupGeneration generation; bool team_reusable{}; };
class TargetedGenerationControl;
class TargetedWorkerGeneration;
enum class TargetedWorkerDisposition : std::uint8_t {
    succeeded,
    caller_failure,
    standard_exception,
    unknown_exception,
    runner_cancelled,
    team_stopped,
    missing_signal,
    duplicate_signal,
    signal_before_release,
    invalid_callback_result
};
struct TargetedWorkerCallbackResult {
    TargetedWorkerDisposition disposition;
    std::uint32_t opaque_token;
};
struct TargetedWorkerExecutionControl {
    std::stop_token team_stop;
    std::stop_token command_stop;
    void signal_timed_completion() const noexcept {
        if (signal)
            signal(context);
    }
    [[nodiscard]] auto cancellation_requested() const noexcept -> bool {
        return cancellation != nullptr && cancellation->requested();
    }
    const RunnerCancellation* cancellation{};
    void* context{};
    void (*signal)(void*) noexcept{};
};
struct TargetedWorkerResult {
    core::WorkerId worker_id;
    TargetedWorkerDisposition disposition;
    std::uint32_t opaque_token;
    bool cancellation_observed;
};
struct TargetedWorkerOperation {
    void* context{};
    TargetedWorkerCallbackResult (*run)(void*, const WorkerExecutionContext&,
                                        const TargetedWorkerExecutionControl&){};
};
struct TargetedWorkerReadinessOperation {
    void* context{};
    std::expected<void, core::Error> (*observe)(void*, const WorkerExecutionContext&) noexcept {};
};
static_assert(std::is_trivial_v<TargetedWorkerCallbackResult>);
static_assert(std::is_standard_layout_v<TargetedWorkerCallbackResult>);
static_assert(std::is_trivially_copyable_v<TargetedWorkerResult>);
enum class ForcedTeamStopTestFault : std::uint8_t {
    none, join_failed, contained_exception, contained_unknown_failure
};
enum class StopEligibilityTestFault : std::uint8_t { before_publication, after_publication };
enum class MonitoringQ1StopExceptionTestFault : std::uint8_t {
    standard_before_eligibility,
    unknown_before_eligibility,
    standard_after_eligibility,
    unknown_after_eligibility
};
enum class CleanupSlotTestFault : std::uint8_t {
    owner_cookie, self_cookie, zero_generation, future_generation, dirty_cleanup
};
enum class TargetedSnapshotTestFault : std::uint8_t {
    spec_index, spec_worker_id, spec_processing_unit, spec_core, spec_stable_pu,
    spec_stable_core, spec_cpu, ready_index, ready_worker_id, ready_stable_pu,
    ready_stable_core, ready_cpu, ready_affinity, ready_observed_cpu
};
struct TargetedStateTestSnapshot {
    std::uint64_t generation;
    std::size_t armed;
    std::size_t timed_completed;
    std::size_t completed;
    bool start_released;
    bool targeted;
    bool control_borrowed;
    std::uint8_t team_state;
    bool team_stop_requested;
    bool command_stop_requested;
    const void* operation_context;
    bool operation_present;
    std::uint8_t control_phase;
    std::uint8_t control_disposition;
    bool control_result_present;
    bool control_error_present;
    bool control_stop_requested;
    std::uint32_t control_token;
    bool cancellation_observed;
    const void* targeted_operation_context;
    bool targeted_operation_present;
    std::uint8_t observation_policy;
    std::uint8_t cancellation_policy;
    std::size_t targeted_index;
    const void* targeted_control_identity;
    bool staged;
    bool generation_cancelled;
    std::uint64_t timed_signalled_mask;
    std::uint64_t pre_timed_error_mask;
    std::uint64_t control_generation;
    bool control_results_collected;
    std::uint64_t command_result_occupied_mask;
    std::uint64_t command_result_success_mask;
    std::uint64_t command_result_before_mask;
    std::uint64_t command_result_after_mask;
    std::uint64_t command_result_migrated_mask;
    std::uint64_t command_result_cancelled_mask;
    std::array<std::uint32_t, 64> command_result_worker_ids;
    std::array<std::uint32_t, 64> command_result_before_cpus;
    std::array<std::uint32_t, 64> command_result_after_cpus;
    std::size_t timed_signal_context_count;
    std::array<const void*, 64> timed_signal_context_states;
    std::array<std::size_t, 64> timed_signal_context_indices;
    std::array<std::uint64_t, 64> timed_signal_context_generations;
    auto operator==(const TargetedStateTestSnapshot&) const -> bool = default;
};

[[nodiscard]] auto production_worker_team_dependencies() noexcept -> WorkerTeamDependencies;

struct PersistentWorkerTeamAccess {
  public:
    [[nodiscard]] static auto project(std::span<const AdmittedWorker> workers) noexcept
        -> std::expected<std::vector<WorkerLaunchSpec>, core::Error>;
    [[nodiscard]] static auto startup(const WorkerLaunchSpec& spec,
                                      const WorkerPlatformOperations& platform) noexcept
        -> std::expected<WorkerReadyEvidence, core::Error>;
    [[nodiscard]] static auto create(std::span<const AdmittedWorker> workers,
                                     RunnerCancellation& cancellation,
                                     WorkerTeamDependencies dependencies) noexcept
        -> std::expected<PersistentWorkerTeam, core::Error>;
    [[nodiscard]] static auto create_from_launch_specs(std::vector<WorkerLaunchSpec>,
                                                       RunnerCancellation&,
                                                       WorkerTeamDependencies) noexcept
        -> std::expected<PersistentWorkerTeam, core::Error>;
    [[nodiscard]] static auto
    execute_for_test(std::span<const WorkerLaunchSpec> specs,
                     std::span<const WorkerReadyEvidence> readiness,
                     RunnerCancellation& cancellation, const WorkerPlatformOperations& platform,
                     WorkerOperation operation, WorkerObservationPolicy observation,
                     WorkerCancellationPolicy cancellation_policy) -> WorkerTeamCommandOutcome;
    [[nodiscard]] static auto probe_launches(std::size_t count,
                                             WorkerThreadLauncher launcher) noexcept
        -> std::expected<std::size_t, core::Error>;
    [[nodiscard]] static auto active_destruction_is_fatal(const PersistentWorkerTeam& team) noexcept
        -> bool;
    [[nodiscard]] static auto force_stop_and_join_after_failure(
        PersistentWorkerTeam& team) noexcept -> ForcedTeamStopResult;
    [[nodiscard]] static auto stop_monitoring_q1_session_noexcept(
        PersistentWorkerTeam&, core::Error, core::Error) noexcept
        -> std::expected<void, core::Error>;
    [[nodiscard]] static auto execute_cleanup_preallocated(
        PersistentWorkerTeam&, CleanupWorkerOperation,
        std::span<CleanupWorkerSlot>) noexcept -> CleanupCommandResult;
    static void reset_cleanup_slots_after_consumption(
        PersistentWorkerTeam&, CleanupGeneration, std::span<CleanupWorkerSlot>) noexcept;
    static void inject_stop_failure_for_test(PersistentWorkerTeam&,
                                             std::uint32_t worker_index) noexcept;
    static void inject_inactive_stop_failure_for_test(PersistentWorkerTeam&) noexcept;
    static void inject_stop_eligibility_failure_for_test(
        PersistentWorkerTeam&,StopEligibilityTestFault) noexcept;
    static void inject_monitoring_q1_stop_exception_for_test(
        PersistentWorkerTeam&, MonitoringQ1StopExceptionTestFault) noexcept;
    static void inject_force_stop_failure_for_test(PersistentWorkerTeam&, std::uint8_t sweep,
                                                   std::uint32_t worker_index,
                                                   ForcedTeamStopTestFault) noexcept;
    static void set_force_error_capacity_for_test(PersistentWorkerTeam&,
                                                  std::size_t capacity) noexcept;
    static void clear_stop_failures_for_test(PersistentWorkerTeam&) noexcept;
    static void set_generation_for_test(PersistentWorkerTeam& team, std::uint64_t generation);
    static void corrupt_targeted_snapshot_for_test(PersistentWorkerTeam&,
                                                   TargetedSnapshotTestFault);
    static void set_targeted_publication_hook_for_test(void* context,
                                                       void (*hook)(void*) noexcept) noexcept;
    static void set_force_interval_hooks_for_test(
        void*,void (*)(void*) noexcept,void (*)(void*) noexcept) noexcept;
    static void signal_targeted_before_release_for_test(PersistentWorkerTeam&) noexcept;
    [[nodiscard]] static auto targeted_team_fatal_for_test(
        const PersistentWorkerTeam&) noexcept -> bool;
    [[nodiscard]] static auto targeted_control_borrowed(
        const TargetedGenerationControl&) noexcept -> bool;
    [[nodiscard]] static auto targeted_state_snapshot_for_test(
        const PersistentWorkerTeam&, const TargetedGenerationControl&) noexcept
        -> TargetedStateTestSnapshot;
    static void set_cleanup_generation_for_test(PersistentWorkerTeam& team,
                                                std::uint64_t generation);
    static void corrupt_cleanup_slot_for_test(CleanupWorkerSlot&, CleanupSlotTestFault) noexcept;
    static void signal_timed_completion(void* context) noexcept;
    [[nodiscard]] static auto transfer_staged_outcome(void* team_state,
                                                      PersistentWorkerGenerationState& generation)
        -> std::optional<core::Error>;
    static void worker_entry(void* state, std::size_t worker_index) noexcept;
    [[nodiscard]] static auto make_targeted_control()
        -> std::expected<TargetedGenerationControl, core::Error>;
    [[nodiscard]] static auto begin_targeted_generation_preallocated(
        const AdmittedPlacementRequest&, PersistentWorkerTeam&, TargetedGenerationControl&,
        std::size_t, TargetedWorkerOperation,
        WorkerCancellationPolicy = WorkerCancellationPolicy::observe)
        -> std::expected<TargetedWorkerGeneration, core::Error>;
    [[nodiscard]] static auto begin_targeted_generation_preallocated(
        std::span<const WorkerLaunchSpec>, PersistentWorkerTeam&, TargetedGenerationControl&,
        std::size_t, TargetedWorkerOperation,
        WorkerCancellationPolicy = WorkerCancellationPolicy::observe)
        -> std::expected<TargetedWorkerGeneration, core::Error>;
    [[nodiscard]] static auto begin_q2_targeted_generation_preallocated(
        std::span<const WorkerLaunchSpec>, PersistentWorkerTeam&, TargetedGenerationControl&,
        std::size_t, TargetedWorkerOperation, TargetedWorkerReadinessOperation,
        WorkerCancellationPolicy = WorkerCancellationPolicy::observe)
        -> std::expected<TargetedWorkerGeneration, core::Error>;
    [[nodiscard]] static auto reset_targeted_control(TargetedGenerationControl&) noexcept
        -> std::expected<void, core::Error>;
};

class TargetedGenerationControl {
  public:
    TargetedGenerationControl(TargetedGenerationControl&&) noexcept;
    TargetedGenerationControl(const TargetedGenerationControl&) = delete;
    auto operator=(const TargetedGenerationControl&) -> TargetedGenerationControl& = delete;
    auto operator=(TargetedGenerationControl&&) -> TargetedGenerationControl& = delete;
    ~TargetedGenerationControl() noexcept;

  private:
    struct State;
    explicit TargetedGenerationControl(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
    friend struct PersistentWorkerTeamAccess;
    friend class TargetedWorkerGeneration;
    friend class ::epycbench::runtime::PersistentWorkerTeam;
};

class TargetedWorkerGeneration {
  public:
    [[nodiscard]] auto await_ready() -> std::expected<void, core::Error>;
    void cancel_before_release() noexcept;
    void release() noexcept;
    [[nodiscard]] auto await_timed_completion() -> std::expected<void, core::Error>;
    [[nodiscard]] auto await_quiescence() -> std::expected<void, core::Error>;
    [[nodiscard]] auto collect_result() -> std::expected<TargetedWorkerResult, core::Error>;
    TargetedWorkerGeneration(TargetedWorkerGeneration&&) noexcept;
    TargetedWorkerGeneration(const TargetedWorkerGeneration&) = delete;
    auto operator=(const TargetedWorkerGeneration&) -> TargetedWorkerGeneration& = delete;
    auto operator=(TargetedWorkerGeneration&&) -> TargetedWorkerGeneration& = delete;
    ~TargetedWorkerGeneration() noexcept;
  private:
    TargetedWorkerGeneration(void*, TargetedGenerationControl::State*) noexcept;
    void* team_{};
    TargetedGenerationControl::State* state_{};
    friend struct PersistentWorkerTeamAccess;
};

} // namespace epycbench::runtime::detail
