#include "epycbench/runtime/persistent_worker_team.hpp"

#include "runtime/worker_team_state.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace epycbench::runtime {
namespace {
void* targeted_publication_hook_context{};
void (*targeted_publication_hook)(void*) noexcept{};

std::array<std::uint32_t, 64> injected_stop_workers{};
std::atomic<std::size_t> injected_stop_worker_count{};
std::atomic<bool> injected_inactive_stop_failure{};
std::atomic<unsigned> injected_stop_eligibility_failure{};
std::atomic<unsigned> injected_monitoring_q1_stop_exception{};
void* force_interval_context{};
void (*force_interval_begin)(void*) noexcept{};
void (*force_interval_end)(void*) noexcept{};
struct InjectedForceFault {
    std::uint8_t sweep{};
    std::uint32_t worker{};
    detail::ForcedTeamStopTestFault kind{detail::ForcedTeamStopTestFault::none};
    bool consumed{};
};
std::array<InjectedForceFault, 64> injected_force_faults{};
std::atomic<std::size_t> injected_force_fault_count{};
std::atomic<std::size_t> injected_force_error_capacity{std::numeric_limits<std::size_t>::max()};
std::atomic_flag injected_fault_script_guard = ATOMIC_FLAG_INIT;
thread_local bool owns_injected_fault_script{};
std::atomic<const void*> injected_fault_script_owner{};

void acquire_injected_fault_script(const void* owner) noexcept {
    if (owner == nullptr) std::terminate();
    if (owns_injected_fault_script) {
        if (injected_fault_script_owner.load() != owner) std::terminate();
        return;
    }
    if (injected_fault_script_guard.test_and_set()) std::terminate();
    injected_fault_script_owner.store(owner);
    owns_injected_fault_script = true;
}

void require_injected_fault_script_owner(const void* owner) noexcept {
    if (injected_fault_script_guard.test() && injected_fault_script_owner.load() != owner)
        std::terminate();
}

auto worker_error(std::string code, std::string_view operation, std::string classification,
                  const core::Error* underlying = nullptr) -> core::Error {
    nlohmann::json context{{"classification", std::move(classification)}};
    if (underlying != nullptr)
        context["underlying_code"] = underlying->code.value();
    std::optional<core::OsError> os_error;
    if (underlying != nullptr && underlying->os_error)
        os_error =
            core::OsError{.value = underlying->os_error->value, .domain = "errno", .message = ""};
    return {.category = core::ErrorCategory::runtime,
            .code = core::StableErrorCode{std::move(code)},
            .message = "Persistent worker-team operation failed",
            .operation = std::string{operation},
            .os_error = std::move(os_error),
            .context = std::move(context),
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_experiment};
}

auto contained_error(bool unknown, std::string_view operation) -> core::Error {
    return worker_error(unknown ? "runtime.worker_team.contained_unknown_failure"
                                : "runtime.worker_team.contained_exception",
                        operation, unknown ? "contained_unknown_failure" : "contained_exception");
}

auto placement_error(std::string classification, const core::Error* underlying = nullptr)
    -> core::Error {
    return worker_error("runtime.worker_team.placement_failed", "worker_startup",
                        std::move(classification), underlying);
}

auto command_error(std::string classification, const core::Error* underlying = nullptr)
    -> core::Error {
    return worker_error("runtime.worker_team.command_failed", "worker_command",
                        std::move(classification), underlying);
}

auto cancelled_result(core::WorkerId worker_id) -> WorkerCommandResult {
    return {.worker_id = worker_id,
            .result = std::unexpected{command_error("runner_cancelled")},
            .cpu_before = std::nullopt,
            .cpu_after = std::nullopt,
            .migrated = false,
            .cancellation_observed = true};
}

} // namespace

void detail::PersistentWorkerTeamAccess::set_targeted_publication_hook_for_test(
    void* context, void (*hook)(void*) noexcept) noexcept {
    targeted_publication_hook_context = context;
    targeted_publication_hook = hook;
}

struct PersistentWorkerTeam::State {
    enum class TeamState { starting, ready, command_active, fatal_failed, stopping, joined };

    explicit State(std::vector<detail::WorkerLaunchSpec> launch_specs,
                   RunnerCancellation& runner_cancellation,
                   detail::WorkerTeamDependencies team_dependencies)
        : specs{std::move(launch_specs)}, cancellation{runner_cancellation},
          dependencies{team_dependencies}, startup(specs.size()), command_results(specs.size()),
          timed_signalled(specs.size()), pre_timed_errors(specs.size()),
          timed_signal_contexts(specs.size()), force_stop_errors(specs.size() * 2U + 3U) {
        readiness.reserve(specs.size());
    }

    struct TimedSignalContext {
        State* state{};
        std::size_t worker_index{};
        std::uint64_t generation{};
    };

    std::vector<detail::WorkerLaunchSpec> specs;
    RunnerCancellation& cancellation;
    detail::WorkerTeamDependencies dependencies;
    std::mutex mutex;
    std::condition_variable condition;
    TeamState state{TeamState::starting};
    std::stop_source team_stop;
    std::stop_source command_stop;
    std::vector<std::jthread> threads;
    std::vector<std::optional<std::expected<WorkerReadyEvidence, core::Error>>> startup;
    std::vector<WorkerReadyEvidence> readiness;
    std::vector<std::optional<WorkerCommandResult>> command_results;
    WorkerOperation operation{};
    detail::TargetedWorkerOperation targeted_operation{};
    detail::TargetedWorkerReadinessOperation targeted_readiness{};
    WorkerObservationPolicy observation{WorkerObservationPolicy::none};
    WorkerCancellationPolicy cancellation_policy{WorkerCancellationPolicy::observe};
    std::uint64_t generation{};
    std::size_t armed{};
    std::size_t timed_completed{};
    std::size_t completed{};
    bool start_released{};
    bool staged{};
    bool generation_cancelled{};
    std::vector<bool> timed_signalled;
    std::vector<std::optional<core::Error>> pre_timed_errors;
    std::vector<TimedSignalContext> timed_signal_contexts;
    detail::PersistentWorkerGenerationState* active_generation{};
    std::optional<core::Error> fatal_error;
    std::vector<detail::ForcedTeamStopError> force_stop_errors;
    std::size_t force_stop_error_count{};
    bool force_stop_error_capacity_exhausted{};
    detail::CleanupWorkerOperation cleanup_operation{};
    std::span<detail::CleanupWorkerSlot> cleanup_slots{};
    bool cleanup_command{};
    std::uint64_t cleanup_generation{};
    bool force_eligible{};
    bool targeted{};
    std::size_t targeted_index{};
    void* targeted_control{};
};

namespace detail {

struct PersistentWorkerGenerationState {
    enum class Phase { arming, ready, released, terminal };

    std::uint64_t generation{};
    Phase phase{Phase::arming};
    bool terminal{};
    bool results_collected{};
    std::optional<WorkerTeamCommandOutcome> outcome;
    std::optional<core::Error> terminal_error;
};

struct TargetedGenerationControl::State {
    enum class Disposition : std::uint8_t {
        pristine,
        succeeded,
        operation_failed,
        standard_exception,
        unknown_exception,
        runner_cancelled,
        team_stopped,
        missing_signal,
        duplicate_signal,
        signal_before_release,
        invalid_callback_result
    };
    enum class Phase { pristine, arming, ready, released, terminal };
    Phase phase{Phase::pristine};
    std::uint64_t generation{};
    bool borrowed{};
    bool results_collected{};
    std::optional<TargetedWorkerResult> result;
    std::optional<core::Error> terminal_error;
    std::stop_source stop;
    Disposition disposition{Disposition::pristine};
    std::uint32_t opaque_token{};
    bool cancellation_observed{};
};

namespace {

auto generation_cancelled_error() -> core::Error {
    return worker_error("runtime.worker_team.generation_cancelled", "worker_generation",
                        "runner_cancelled");
}

auto generation_state_error(std::string classification) -> core::Error {
    return worker_error("runtime.worker_team.invalid_state", "worker_generation",
                        std::move(classification));
}

} // namespace

void PersistentWorkerTeamAccess::signal_timed_completion(void* opaque) noexcept {
    auto& signal = *static_cast<PersistentWorkerTeam::State::TimedSignalContext*>(opaque);
    auto& state = *signal.state;
    std::lock_guard lock{state.mutex};
    const auto invalid = state.state != PersistentWorkerTeam::State::TeamState::command_active ||
                         !state.staged || state.generation != signal.generation ||
                         !state.start_released;
    if (invalid) {
        if (state.targeted && state.targeted_control) {
            auto& targeted = *static_cast<TargetedGenerationControl::State*>(state.targeted_control);
            if (targeted.disposition == TargetedGenerationControl::State::Disposition::pristine)
                targeted.disposition =
                    TargetedGenerationControl::State::Disposition::signal_before_release;
            state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
            state.team_stop.request_stop();
            static_cast<TargetedGenerationControl::State*>(state.targeted_control)->stop.request_stop();
            state.condition.notify_all();
            return;
        }
        state.fatal_error = worker_error("runtime.worker_team.invariant_failed",
                                         "worker_generation", "timed_signal_before_release");
        state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
        state.team_stop.request_stop();
        state.command_stop.request_stop();
        if (state.targeted_control)
            static_cast<TargetedGenerationControl::State*>(state.targeted_control)->stop.request_stop();
        state.condition.notify_all();
        return;
    }
    if (state.timed_signalled[signal.worker_index]) {
        if (state.targeted && state.targeted_control) {
            auto& targeted = *static_cast<TargetedGenerationControl::State*>(state.targeted_control);
            if (targeted.disposition == TargetedGenerationControl::State::Disposition::pristine ||
                targeted.disposition == TargetedGenerationControl::State::Disposition::succeeded)
                targeted.disposition = TargetedGenerationControl::State::Disposition::duplicate_signal;
            state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
            state.team_stop.request_stop();
            static_cast<TargetedGenerationControl::State*>(state.targeted_control)->stop.request_stop();
            state.condition.notify_all();
            return;
        }
        state.fatal_error = worker_error("runtime.worker_team.invariant_failed",
                                         "worker_generation", "duplicate_timed_completion_signal");
        state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
        state.team_stop.request_stop();
        state.command_stop.request_stop();
        if (state.targeted_control)
            static_cast<TargetedGenerationControl::State*>(state.targeted_control)->stop.request_stop();
        state.condition.notify_all();
        return;
    }
    state.timed_signalled[signal.worker_index] = true;
    ++state.timed_completed;
    state.condition.notify_all();
}

void PersistentWorkerTeamAccess::signal_targeted_before_release_for_test(
    PersistentWorkerTeam& team) noexcept {
    signal_timed_completion(&team.state_->timed_signal_contexts[team.state_->targeted_index]);
}

auto PersistentWorkerTeamAccess::targeted_team_fatal_for_test(
    const PersistentWorkerTeam& team) noexcept -> bool {
    std::lock_guard lock{team.state_->mutex};
    return team.state_->state == PersistentWorkerTeam::State::TeamState::fatal_failed;
}

auto PersistentWorkerTeamAccess::targeted_control_borrowed(
    const TargetedGenerationControl& control) noexcept -> bool {
    return control.state_ && control.state_->borrowed;
}

auto PersistentWorkerTeamAccess::targeted_state_snapshot_for_test(
    const PersistentWorkerTeam& team, const TargetedGenerationControl& control) noexcept
    -> TargetedStateTestSnapshot {
    std::lock_guard lock{team.state_->mutex};
    std::uint64_t signal_mask{};
    std::uint64_t error_mask{};
    std::uint64_t result_mask{};
    std::uint64_t success_mask{};
    std::uint64_t before_mask{};
    std::uint64_t after_mask{};
    std::uint64_t migrated_mask{};
    std::uint64_t cancelled_mask{};
    std::array<std::uint32_t, 64> result_worker_ids{};
    std::array<std::uint32_t, 64> result_before_cpus{};
    std::array<std::uint32_t, 64> result_after_cpus{};
    std::array<const void*, 64> signal_states{};
    std::array<std::size_t, 64> signal_indices{};
    std::array<std::uint64_t, 64> signal_generations{};
    for (std::size_t index = 0; index < team.state_->timed_signalled.size() && index < 64; ++index) {
        if (team.state_->timed_signalled[index]) signal_mask |= std::uint64_t{1} << index;
        if (team.state_->pre_timed_errors[index]) error_mask |= std::uint64_t{1} << index;
        if (team.state_->command_results[index]) {
            const auto& result = *team.state_->command_results[index];
            result_mask |= std::uint64_t{1} << index;
            if (result.result) success_mask |= std::uint64_t{1} << index;
            if (result.cpu_before) {
                before_mask |= std::uint64_t{1} << index;
                result_before_cpus[index] = result.cpu_before->value();
            }
            if (result.cpu_after) {
                after_mask |= std::uint64_t{1} << index;
                result_after_cpus[index] = result.cpu_after->value();
            }
            if (result.migrated) migrated_mask |= std::uint64_t{1} << index;
            if (result.cancellation_observed) cancelled_mask |= std::uint64_t{1} << index;
            result_worker_ids[index] = result.worker_id.value();
        }
        const auto& context = team.state_->timed_signal_contexts[index];
        signal_states[index] = context.state;
        signal_indices[index] = context.worker_index;
        signal_generations[index] = context.generation;
    }
    return {.generation = team.state_->generation,
            .armed = team.state_->armed,
            .timed_completed = team.state_->timed_completed,
            .completed = team.state_->completed,
            .start_released = team.state_->start_released,
            .targeted = team.state_->targeted,
            .control_borrowed = control.state_ && control.state_->borrowed,
            .team_state = static_cast<std::uint8_t>(team.state_->state),
            .team_stop_requested = team.state_->team_stop.stop_requested(),
            .command_stop_requested = team.state_->command_stop.stop_requested(),
            .operation_context = team.state_->operation.context,
            .operation_present = team.state_->operation.run != nullptr,
            .control_phase = static_cast<std::uint8_t>(control.state_
                                                           ? control.state_->phase
                                                           : TargetedGenerationControl::State::Phase::pristine),
            .control_disposition = static_cast<std::uint8_t>(
                control.state_ ? control.state_->disposition
                               : TargetedGenerationControl::State::Disposition::pristine),
            .control_result_present = control.state_ && control.state_->result.has_value(),
            .control_error_present = control.state_ && control.state_->terminal_error.has_value(),
            .control_stop_requested = control.state_ && control.state_->stop.stop_requested(),
            .control_token = control.state_ ? control.state_->opaque_token : 0,
            .cancellation_observed = control.state_ && control.state_->cancellation_observed,
            .targeted_operation_context = team.state_->targeted_operation.context,
            .targeted_operation_present = team.state_->targeted_operation.run != nullptr,
            .observation_policy = static_cast<std::uint8_t>(team.state_->observation),
            .cancellation_policy = static_cast<std::uint8_t>(team.state_->cancellation_policy),
            .targeted_index = team.state_->targeted_index,
            .targeted_control_identity = team.state_->targeted_control,
            .staged = team.state_->staged,
            .generation_cancelled = team.state_->generation_cancelled,
            .timed_signalled_mask = signal_mask,
            .pre_timed_error_mask = error_mask,
            .control_generation = control.state_ ? control.state_->generation : 0,
            .control_results_collected = control.state_ && control.state_->results_collected,
            .command_result_occupied_mask = result_mask,
            .command_result_success_mask = success_mask,
            .command_result_before_mask = before_mask,
            .command_result_after_mask = after_mask,
            .command_result_migrated_mask = migrated_mask,
            .command_result_cancelled_mask = cancelled_mask,
            .command_result_worker_ids = result_worker_ids,
            .command_result_before_cpus = result_before_cpus,
            .command_result_after_cpus = result_after_cpus,
            .timed_signal_context_count = std::min<std::size_t>(
                team.state_->timed_signal_contexts.size(), 64),
            .timed_signal_context_states = signal_states,
            .timed_signal_context_indices = signal_indices,
            .timed_signal_context_generations = signal_generations};
}

auto PersistentWorkerTeamAccess::transfer_staged_outcome(
    void* opaque, PersistentWorkerGenerationState& generation) -> std::optional<core::Error> {
    auto& team = *static_cast<PersistentWorkerTeam::State*>(opaque);
    WorkerTeamCommandOutcome outcome;
    outcome.workers.reserve(team.command_results.size());
    for (auto& slot : team.command_results) {
        if (!slot)
            return worker_error("runtime.worker_team.invariant_failed", "worker_generation",
                                "unexpected_worker_state");
        outcome.workers.push_back(std::move(*slot));
    }
    generation.outcome = std::move(outcome);
    return std::nullopt;
}

namespace {

auto production_tid(void*) -> std::expected<platform_linux::LinuxThreadId, core::Error> {
    return platform_linux::current_thread_id();
}
auto production_set(void*, const core::CpuSet& cpus) -> std::expected<void, core::Error> {
    return platform_linux::set_current_thread_affinity(cpus);
}
auto production_get(void*) -> std::expected<core::CpuSet, core::Error> {
    return platform_linux::get_current_thread_affinity();
}
auto production_cpu(void*) -> std::expected<core::LinuxCpuId, core::Error> {
    return platform_linux::current_cpu();
}

auto production_launch(void*, WorkerThreadEntry entry, void* state, std::size_t worker_index)
    -> std::expected<std::jthread, core::Error> {
    try {
        return std::jthread{entry, state, worker_index};
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "worker_launch")};
    } catch (...) {
        return std::unexpected{contained_error(true, "worker_launch")};
    }
}

auto all_startup_reported(const auto& state) -> bool {
    return std::ranges::all_of(state.startup, [](const auto& slot) { return slot.has_value(); });
}

auto lowest_startup_error(const auto& state,
                          const std::optional<std::pair<std::size_t, core::Error>>& launch_error)
    -> core::Error {
    std::size_t selected = launch_error ? launch_error->first : state.specs.size();
    const core::Error* error = launch_error ? &launch_error->second : nullptr;
    for (std::size_t index = 0; index < state.startup.size(); ++index) {
        if (state.startup[index] && !state.startup[index]->has_value() && index < selected) {
            selected = index;
            error = &state.startup[index]->error();
        }
    }
    if (error != nullptr)
        return worker_error("runtime.worker_team.start_failed", "worker_team_create",
                            launch_error && selected == launch_error->first
                                ? "thread_launch_failed"
                                : error->context.value("classification", "worker_not_ready"),
                            error);
    return worker_error("runtime.worker_team.start_failed", "worker_team_create",
                        "worker_not_ready");
}

auto run_worker_command(auto& state, std::size_t index, WorkerOperation operation,
                        WorkerObservationPolicy observation,
                        WorkerCancellationPolicy cancellation_policy, std::stop_token command_stop,
                        bool staged = false) -> WorkerCommandResult {
    const auto& spec = state.specs[index];
    const auto& ready = state.readiness[index];
    WorkerCommandResult result{.worker_id = spec.worker_id,
                               .result = {},
                               .cpu_before = std::nullopt,
                               .cpu_after = std::nullopt,
                               .migrated = false,
                               .cancellation_observed = false};

    const auto observe_cpu = [&]() {
        return state.dependencies.platform.current_cpu(state.dependencies.platform.context);
    };
    bool invoke = true;
    if (observation == WorkerObservationPolicy::before_and_after) {
        auto before = observe_cpu();
        if (!before) {
            result.result = std::unexpected{command_error("current_cpu_failed", &before.error())};
            invoke = false;
        } else {
            result.cpu_before = *before;
            if (*before != spec.requested_cpu) {
                result.migrated = true;
                result.result = std::unexpected{command_error("worker_migrated")};
                invoke = false;
            }
        }
        if (!invoke)
            state.command_stop.request_stop();
    }

    if (state.team_stop.stop_requested()) {
        if (result.result)
            result.result = std::unexpected{command_error("team_stopped")};
        invoke = false;
    }
    const bool runner_cancelled = state.cancellation.requested();
    if (cancellation_policy == WorkerCancellationPolicy::observe && runner_cancelled) {
        result.cancellation_observed = true;
        if (result.result)
            result.result = std::unexpected{command_error("runner_cancelled")};
        invoke = false;
    }

    if (invoke) {
        const WorkerExecutionContext worker{.worker_index = spec.worker_index,
                                            .worker_id = spec.worker_id,
                                            .processing_unit = spec.processing_unit,
                                            .core = spec.core,
                                            .requested_cpu = spec.requested_cpu,
                                            .readiness = ready};
        auto* timed_context = staged ? &state.timed_signal_contexts[index] : nullptr;
        const WorkerExecutionControl control{
            state.cancellation, state.team_stop.get_token(), command_stop, timed_context,
            staged ? PersistentWorkerTeamAccess::signal_timed_completion : nullptr};
        try {
            auto operation_result = operation.run(operation.context, worker, control);
            if (!operation_result) {
                result.result = std::unexpected{
                    staged ? std::move(operation_result.error())
                           : command_error("worker_operation_failed", &operation_result.error())};
                state.command_stop.request_stop();
            }
        } catch (const std::exception&) {
            result.result = std::unexpected{contained_error(false, "worker_callback")};
            state.command_stop.request_stop();
        } catch (...) {
            result.result = std::unexpected{contained_error(true, "worker_callback")};
            state.command_stop.request_stop();
        }
    }

    if (staged) {
        std::lock_guard lock{state.mutex};
        if (!state.timed_signalled[index]) {
            if (result.result) {
                state.fatal_error =
                    worker_error("runtime.worker_team.invariant_failed", "worker_generation",
                                 "missing_timed_completion_signal");
                state.state = decltype(state.state)::fatal_failed;
                state.team_stop.request_stop();
                state.command_stop.request_stop();
            } else {
                state.pre_timed_errors[index] = result.result.error();
                state.command_stop.request_stop();
            }
            state.condition.notify_all();
        }
    }

    if (observation == WorkerObservationPolicy::before_and_after) {
        auto after = observe_cpu();
        if (!after) {
            if (result.result)
                result.result =
                    std::unexpected{command_error("current_cpu_failed", &after.error())};
        } else {
            result.cpu_after = *after;
            if (*after != spec.requested_cpu ||
                (result.cpu_before && *result.cpu_before != *after)) {
                result.migrated = true;
                if (result.result)
                    result.result = std::unexpected{command_error("worker_migrated")};
            }
        }
    }
    if (state.cancellation.requested())
        result.cancellation_observed = true;
    return result;
}

} // namespace

auto production_worker_platform_operations() noexcept -> WorkerPlatformOperations {
    return {.context = nullptr,
            .current_thread_id = production_tid,
            .set_current_thread_affinity = production_set,
            .get_current_thread_affinity = production_get,
            .current_cpu = production_cpu};
}

auto production_worker_thread_launcher() noexcept -> WorkerThreadLauncher {
    return {.context = nullptr, .launch = production_launch};
}

auto production_worker_team_dependencies() noexcept -> WorkerTeamDependencies {
    return {.platform = production_worker_platform_operations(),
            .launcher = production_worker_thread_launcher()};
}

auto PersistentWorkerTeamAccess::project(std::span<const AdmittedWorker> workers) noexcept
    -> std::expected<std::vector<WorkerLaunchSpec>, core::Error> {
    try {
        if (workers.empty())
            return std::unexpected{worker_error("runtime.worker_team.invalid_argument",
                                                "worker_team_create", "empty_worker_set")};
        constexpr auto maximum_worker_id =
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
        if (workers.size() - 1U > maximum_worker_id)
            return std::unexpected{worker_error("runtime.worker_team.invalid_argument",
                                                "worker_team_create", "worker_id_overflow")};

        std::set<core::ProcessingUnitId> processing_units;
        std::set<core::LinuxCpuId> cpus;
        std::vector<WorkerLaunchSpec> result;
        result.reserve(workers.size());
        for (std::size_t index = 0; index < workers.size(); ++index) {
            const auto& admitted = workers[index];
            if (!processing_units.insert(admitted.processing_unit()).second)
                return std::unexpected{worker_error("runtime.worker_team.invariant_failed",
                                                    "worker_team_create", "duplicate_worker")};
            const auto cpu = admitted.request().cpu().linux_cpu();
            if (!cpus.insert(cpu).second)
                return std::unexpected{worker_error("runtime.worker_team.invariant_failed",
                                                    "worker_team_create", "duplicate_cpu")};
            result.push_back({.worker_index = index,
                              .worker_id = core::WorkerId{static_cast<std::uint32_t>(index)},
                              .processing_unit = admitted.processing_unit(),
                              .core = admitted.core(),
                              .stable_pu = admitted.request().cpu().stable_pu_id(),
                              .stable_core = admitted.request().cpu().stable_core_id(),
                              .requested_cpu = cpu});
        }
        return result;
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "worker_projection")};
    } catch (...) {
        return std::unexpected{contained_error(true, "worker_projection")};
    }
}

auto PersistentWorkerTeamAccess::startup(const WorkerLaunchSpec& spec,
                                         const WorkerPlatformOperations& platform) noexcept
    -> std::expected<WorkerReadyEvidence, core::Error> {
    try {
        auto tid = platform.current_thread_id(platform.context);
        if (!tid)
            return std::unexpected{placement_error("worker_identity_failed", &tid.error())};
        const core::CpuSet requested{spec.requested_cpu};
        auto applied = platform.set_current_thread_affinity(platform.context, requested);
        if (!applied)
            return std::unexpected{placement_error("affinity_apply_failed", &applied.error())};
        auto affinity = platform.get_current_thread_affinity(platform.context);
        if (!affinity)
            return std::unexpected{placement_error("affinity_readback_failed", &affinity.error())};
        if (*affinity != requested)
            return std::unexpected{placement_error("affinity_mismatch")};
        auto cpu = platform.current_cpu(platform.context);
        if (!cpu)
            return std::unexpected{placement_error("current_cpu_failed", &cpu.error())};
        if (*cpu != spec.requested_cpu)
            return std::unexpected{placement_error("current_cpu_mismatch")};
        return WorkerReadyEvidence{.worker_index = spec.worker_index,
                                   .worker_id = spec.worker_id,
                                   .stable_pu = spec.stable_pu,
                                   .stable_core = spec.stable_core,
                                   .requested_cpu = spec.requested_cpu,
                                   .tid = *tid,
                                   .observed_affinity = std::move(*affinity),
                                   .observed_cpu = *cpu};
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "worker_startup")};
    } catch (...) {
        return std::unexpected{contained_error(true, "worker_startup")};
    }
}

void PersistentWorkerTeamAccess::worker_entry(void* opaque, std::size_t worker_index) noexcept {
    auto& state = *static_cast<PersistentWorkerTeam::State*>(opaque);
    auto started = startup(state.specs[worker_index], state.dependencies.platform);
    {
        std::lock_guard lock{state.mutex};
        state.startup[worker_index] = std::move(started);
        if (!state.startup[worker_index]->has_value()) {
            state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
            state.team_stop.request_stop();
        }
    }
    state.condition.notify_all();

    std::uint64_t observed_generation{};
    for (;;) {
        WorkerOperation operation;
        TargetedWorkerOperation targeted_operation;
        TargetedWorkerReadinessOperation targeted_readiness;
        WorkerObservationPolicy observation;
        WorkerCancellationPolicy cancellation_policy;
        CleanupWorkerOperation cleanup_operation;
        std::stop_token command_stop;
        bool staged{};
        bool cleanup_command{};
        bool targeted_command{};
        {
            std::unique_lock lock{state.mutex};
            state.condition.wait(lock, [&] {
                return state.team_stop.stop_requested() ||
                       (state.state == PersistentWorkerTeam::State::TeamState::command_active &&
                        state.generation != observed_generation);
            });
            if (state.team_stop.stop_requested())
                return;
            observed_generation = state.generation;
            if (state.targeted && worker_index != state.targeted_index)
                continue;
            operation = state.operation;
            targeted_operation = state.targeted_operation;
            targeted_readiness = state.targeted_readiness;
            targeted_command = state.targeted;
            observation = state.observation;
            cancellation_policy = state.cancellation_policy;
            command_stop = state.targeted
                               ? static_cast<TargetedGenerationControl::State*>(
                                     state.targeted_control)->stop.get_token()
                               : state.command_stop.get_token();
            staged = state.staged;
            cleanup_command = state.cleanup_command;
            cleanup_operation = state.cleanup_operation;
            if (targeted_command && targeted_readiness.observe) {
                const auto& spec = state.specs[worker_index];
                const auto& ready = state.readiness[worker_index];
                const WorkerExecutionContext worker{spec.worker_index, spec.worker_id,
                    spec.processing_unit, spec.core, spec.requested_cpu, ready};
                auto observed = targeted_readiness.observe(targeted_readiness.context, worker);
                if (!observed) {
                    auto& control = *static_cast<TargetedGenerationControl::State*>(state.targeted_control);
                    control.terminal_error.emplace(std::move(observed.error()));
                    state.generation_cancelled = true;
                }
            }
            ++state.armed;
            state.condition.notify_all();
            state.condition.wait(lock, [&] {
                return state.start_released || state.generation_cancelled ||
                       state.team_stop.stop_requested();
            });
            if (state.generation_cancelled) {
                ++state.completed;
                state.condition.notify_all();
                continue;
            }
            if (state.team_stop.stop_requested() && !state.start_released) {
                if (staged) {
                    ++state.completed;
                    state.condition.notify_all();
                }
                return;
            }
        }

        if (cleanup_command) {
            auto& slot = state.cleanup_slots[worker_index];
            const auto& spec = state.specs[worker_index];
            const auto& ready = state.readiness[worker_index];
            const WorkerExecutionContext worker{.worker_index = spec.worker_index,
                                                .worker_id = spec.worker_id,
                                                .processing_unit = spec.processing_unit,
                                                .core = spec.core,
                                                .requested_cpu = spec.requested_cpu,
                                                .readiness = ready};
            const WorkerExecutionControl control{state.cancellation,
                                                 state.team_stop.get_token(),
                                                 command_stop, nullptr, nullptr};
            try {
                auto result = cleanup_operation.run(cleanup_operation.context, worker, control);
                if (result) {
                    slot.disposition = CleanupWorkerDisposition::succeeded;
                } else {
                    slot.primary.emplace(std::move(result.error().primary));
                    if (result.error().cleanup)
                        slot.cleanup.emplace(std::move(*result.error().cleanup));
                    slot.disposition = CleanupWorkerDisposition::primary_failure;
                }
            } catch (const std::exception&) {
                slot.disposition = CleanupWorkerDisposition::contained_exception;
            } catch (...) {
                slot.disposition = CleanupWorkerDisposition::contained_unknown;
            }
            {
                std::lock_guard lock{state.mutex};
                ++state.completed;
            }
            state.condition.notify_all();
            continue;
        }

        if (targeted_command) {
            auto& targeted_state = *static_cast<TargetedGenerationControl::State*>(
                state.targeted_control);
            const auto& spec = state.specs[worker_index];
            const auto& ready = state.readiness[worker_index];
            const WorkerExecutionContext worker{.worker_index = spec.worker_index,
                                                .worker_id = spec.worker_id,
                                                .processing_unit = spec.processing_unit,
                                                .core = spec.core,
                                                .requested_cpu = spec.requested_cpu,
                                                .readiness = ready};
            if (state.team_stop.stop_requested() &&
                targeted_state.disposition == TargetedGenerationControl::State::Disposition::pristine)
                targeted_state.disposition = TargetedGenerationControl::State::Disposition::team_stopped;
            else if (cancellation_policy == WorkerCancellationPolicy::observe &&
                     state.cancellation.requested())
                targeted_state.disposition =
                    TargetedGenerationControl::State::Disposition::runner_cancelled;
            if (targeted_state.disposition == TargetedGenerationControl::State::Disposition::pristine) {
                const TargetedWorkerExecutionControl control{
                    .team_stop = state.team_stop.get_token(),
                    .command_stop = command_stop,
                    .cancellation = &state.cancellation,
                    .context = &state.timed_signal_contexts[worker_index],
                    .signal = PersistentWorkerTeamAccess::signal_timed_completion};
                try {
                    const auto callback =
                        targeted_operation.run(targeted_operation.context, worker, control);
                    if (targeted_state.disposition !=
                        TargetedGenerationControl::State::Disposition::pristine) {
                        // A fatal signal disposition published synchronously by this callback wins.
                    } else if ((callback.disposition == TargetedWorkerDisposition::succeeded &&
                         callback.opaque_token == 0) ||
                        (callback.disposition == TargetedWorkerDisposition::caller_failure &&
                         callback.opaque_token != 0)) {
                        targeted_state.disposition =
                            callback.disposition == TargetedWorkerDisposition::succeeded
                                ? TargetedGenerationControl::State::Disposition::succeeded
                                : TargetedGenerationControl::State::Disposition::operation_failed;
                        targeted_state.opaque_token = callback.opaque_token;
                    } else {
                        targeted_state.disposition =
                            TargetedGenerationControl::State::Disposition::invalid_callback_result;
                    }
                } catch (const std::exception&) {
                    if (targeted_state.disposition ==
                        TargetedGenerationControl::State::Disposition::pristine)
                        targeted_state.disposition =
                            TargetedGenerationControl::State::Disposition::standard_exception;
                } catch (...) {
                    if (targeted_state.disposition ==
                        TargetedGenerationControl::State::Disposition::pristine)
                        targeted_state.disposition =
                            TargetedGenerationControl::State::Disposition::unknown_exception;
                }
            }
            targeted_state.cancellation_observed = state.cancellation.requested();
            if (targeted_state.cancellation_observed &&
                cancellation_policy == WorkerCancellationPolicy::observe &&
                targeted_state.disposition == TargetedGenerationControl::State::Disposition::succeeded)
                targeted_state.disposition =
                    TargetedGenerationControl::State::Disposition::runner_cancelled;
            {
                std::lock_guard lock{state.mutex};
                if (!state.timed_signalled[worker_index] &&
                    targeted_state.disposition == TargetedGenerationControl::State::Disposition::succeeded) {
                    targeted_state.disposition = TargetedGenerationControl::State::Disposition::missing_signal;
                    state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
                    state.team_stop.request_stop();
                    targeted_state.stop.request_stop();
                }
                if (targeted_state.disposition ==
                    TargetedGenerationControl::State::Disposition::invalid_callback_result) {
                    state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
                    state.team_stop.request_stop();
                    targeted_state.stop.request_stop();
                }
                ++state.completed;
            }
            state.condition.notify_all();
            continue;
        }

        try {
            auto result = run_worker_command(state, worker_index, operation, observation,
                                             cancellation_policy, command_stop, staged);
            {
                std::lock_guard lock{state.mutex};
                state.command_results[worker_index] = std::move(result);
                ++state.completed;
            }
            state.condition.notify_all();
        } catch (const std::exception&) {
            {
                std::lock_guard lock{state.mutex};
                state.fatal_error = contained_error(false, "worker_result_publication");
                state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
                state.team_stop.request_stop();
                state.command_stop.request_stop();
            }
            state.condition.notify_all();
            return;
        } catch (...) {
            {
                std::lock_guard lock{state.mutex};
                state.fatal_error = contained_error(true, "worker_result_publication");
                state.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
                state.team_stop.request_stop();
                state.command_stop.request_stop();
            }
            state.condition.notify_all();
            return;
        }
    }
}

auto PersistentWorkerTeamAccess::create(std::span<const AdmittedWorker> workers,
                                        RunnerCancellation& cancellation,
                                        WorkerTeamDependencies dependencies) noexcept
    -> std::expected<PersistentWorkerTeam, core::Error> {
    auto projected = project(workers);
    if (!projected)
        return std::unexpected{std::move(projected.error())};
    return create_from_launch_specs(std::move(*projected), cancellation, std::move(dependencies));
}

auto PersistentWorkerTeamAccess::create_from_launch_specs(
    std::vector<WorkerLaunchSpec> specs, RunnerCancellation& cancellation,
    WorkerTeamDependencies dependencies) noexcept
    -> std::expected<PersistentWorkerTeam, core::Error> {
    try {
        auto state = std::make_unique<PersistentWorkerTeam::State>(std::move(specs),
                                                                   cancellation, dependencies);
        state->threads.reserve(state->specs.size());
        std::optional<std::pair<std::size_t, core::Error>> launch_error;
        for (std::size_t index = 0; index < state->specs.size(); ++index) {
            auto launched = dependencies.launcher.launch(dependencies.launcher.context,
                                                         worker_entry, state.get(), index);
            if (!launched) {
                launch_error.emplace(index, std::move(launched.error()));
                break;
            }
            state->threads.push_back(std::move(*launched));
        }

        if (!launch_error) {
            std::unique_lock lock{state->mutex};
            state->condition.wait(lock, [&] {
                return all_startup_reported(*state) || state->team_stop.stop_requested();
            });
        }

        bool failed = launch_error.has_value();
        {
            std::lock_guard lock{state->mutex};
            failed = failed || std::ranges::any_of(state->startup, [](const auto& slot) {
                         return slot && !slot->has_value();
                     });
            if (!failed) {
                state->readiness.reserve(state->startup.size());
                for (auto& slot : state->startup)
                    state->readiness.push_back(std::move(slot->value()));
                state->state = PersistentWorkerTeam::State::TeamState::ready;
            } else {
                state->state = PersistentWorkerTeam::State::TeamState::fatal_failed;
                state->team_stop.request_stop();
            }
        }
        state->condition.notify_all();

        if (failed) {
            for (auto& thread : state->threads) {
                thread.request_stop();
                if (thread.joinable())
                    thread.join();
            }
            return std::unexpected{lowest_startup_error(*state, launch_error)};
        }
        return PersistentWorkerTeam{std::move(state)};
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "worker_team_create")};
    } catch (...) {
        return std::unexpected{contained_error(true, "worker_team_create")};
    }
}

auto PersistentWorkerTeamAccess::execute_for_test(
    std::span<const WorkerLaunchSpec> specs, std::span<const WorkerReadyEvidence> readiness,
    RunnerCancellation& cancellation, const WorkerPlatformOperations& platform,
    WorkerOperation operation, WorkerObservationPolicy observation,
    WorkerCancellationPolicy cancellation_policy) -> WorkerTeamCommandOutcome {
    PersistentWorkerTeam::State state{std::vector<WorkerLaunchSpec>{specs.begin(), specs.end()},
                                      cancellation,
                                      WorkerTeamDependencies{.platform = platform, .launcher = {}}};
    state.readiness.assign(readiness.begin(), readiness.end());
    state.command_stop = std::stop_source{};
    WorkerTeamCommandOutcome outcome;
    outcome.workers.reserve(specs.size());
    for (std::size_t index = 0; index < specs.size(); ++index) {
        outcome.workers.push_back(run_worker_command(state, index, operation, observation,
                                                     cancellation_policy,
                                                     state.command_stop.get_token()));
    }
    return outcome;
}

auto PersistentWorkerTeamAccess::probe_launches(std::size_t count,
                                                WorkerThreadLauncher launcher) noexcept
    -> std::expected<std::size_t, core::Error> {
    std::vector<std::jthread> owners;
    owners.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto launched = launcher.launch(launcher.context, nullptr, nullptr, index);
        if (!launched)
            return std::unexpected{worker_error("runtime.worker_team.start_failed", "worker_launch",
                                                "thread_launch_failed", &launched.error())};
        owners.push_back(std::move(*launched));
    }
    return owners.size();
}

auto PersistentWorkerTeamAccess::active_destruction_is_fatal(
    const PersistentWorkerTeam& team) noexcept -> bool {
    return team.state_ && std::ranges::any_of(team.state_->threads,
                                              [](const auto& thread) { return thread.joinable(); });
}

void PersistentWorkerTeamAccess::set_generation_for_test(PersistentWorkerTeam& team,
                                                         std::uint64_t generation) {
    std::lock_guard lock{team.state_->mutex};
    team.state_->generation = generation;
}

void PersistentWorkerTeamAccess::set_cleanup_generation_for_test(PersistentWorkerTeam& team,
                                                                 std::uint64_t generation) {
    std::lock_guard lock{team.state_->mutex};
    team.state_->cleanup_generation = generation;
}

void PersistentWorkerTeamAccess::corrupt_cleanup_slot_for_test(
    CleanupWorkerSlot& slot, CleanupSlotTestFault fault) noexcept {
    switch (fault) {
    case CleanupSlotTestFault::owner_cookie:
        slot.owner_state_cookie_ = &slot;
        return;
    case CleanupSlotTestFault::self_cookie:
        slot.self_address_cookie_ = nullptr;
        return;
    case CleanupSlotTestFault::zero_generation:
        slot.cleanup_generation_ = 0;
        return;
    case CleanupSlotTestFault::future_generation:
        ++slot.cleanup_generation_;
        return;
    case CleanupSlotTestFault::dirty_cleanup:
        slot.cleanup = worker_error("runtime.worker_team.test", "cleanup_test", "dirty_cleanup");
        return;
    }
    std::terminate();
}

} // namespace detail

auto WorkerTeamCommandOutcome::succeeded() const noexcept -> bool {
    return std::ranges::all_of(workers,
                               [](const auto& worker) { return worker.result.has_value(); });
}

auto WorkerTeamCommandOutcome::cancelled() const noexcept -> bool {
    return std::ranges::any_of(workers,
                               [](const auto& worker) { return worker.cancellation_observed; });
}

PersistentWorkerTeam::PersistentWorkerTeam(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

PersistentWorkerTeam::PersistentWorkerTeam(PersistentWorkerTeam&&) noexcept = default;

PersistentWorkerTeam::~PersistentWorkerTeam() noexcept {
    if (detail::PersistentWorkerTeamAccess::active_destruction_is_fatal(*this))
        std::terminate();
}

auto PersistentWorkerTeam::create(std::span<const AdmittedWorker> workers,
                                  RunnerCancellation& cancellation) noexcept
    -> std::expected<PersistentWorkerTeam, core::Error> {
    return detail::PersistentWorkerTeamAccess::create(
        workers, cancellation, detail::production_worker_team_dependencies());
}

auto PersistentWorkerTeam::readiness() const noexcept -> std::span<const WorkerReadyEvidence> {
    if (!state_)
        return {};
    return state_->readiness;
}

auto PersistentWorkerTeam::execute(WorkerOperation operation, WorkerObservationPolicy observation,
                                   WorkerCancellationPolicy cancellation_policy)
    -> std::expected<WorkerTeamCommandOutcome, core::Error> {
    if (!state_)
        return std::unexpected{
            worker_error("runtime.worker_team.invalid_state", "worker_execute", "team_not_ready")};
    if (operation.run == nullptr)
        return std::unexpected{worker_error("runtime.worker_team.invalid_argument",
                                            "worker_execute", "missing_operation")};

    try {
        std::unique_lock lock{state_->mutex};
        if (state_->state != State::TeamState::ready) {
            const auto classification = state_->state == State::TeamState::command_active
                                            ? "command_active"
                                        : state_->state == State::TeamState::joined ||
                                                state_->state == State::TeamState::stopping
                                            ? "command_after_stop"
                                            : "team_not_ready";
            return std::unexpected{worker_error("runtime.worker_team.invalid_state",
                                                "worker_execute", classification)};
        }
        if (cancellation_policy == WorkerCancellationPolicy::observe &&
            state_->cancellation.requested()) {
            WorkerTeamCommandOutcome outcome;
            outcome.workers.reserve(state_->specs.size());
            for (const auto& spec : state_->specs)
                outcome.workers.push_back(cancelled_result(spec.worker_id));
            return outcome;
        }
        if (state_->generation == std::numeric_limits<std::uint64_t>::max()) {
            state_->state = State::TeamState::fatal_failed;
            state_->fatal_error = worker_error("runtime.worker_team.invariant_failed",
                                               "worker_execute", "generation_exhausted");
            state_->team_stop.request_stop();
            lock.unlock();
            state_->condition.notify_all();
            return std::unexpected{*state_->fatal_error};
        }

        state_->targeted = false;
        state_->targeted_control = nullptr;
        state_->operation = operation;
        state_->observation = observation;
        state_->cancellation_policy = cancellation_policy;
        state_->command_stop = std::stop_source{};
        std::ranges::fill(state_->command_results, std::nullopt);
        state_->armed = 0;
        state_->timed_completed = 0;
        state_->completed = 0;
        state_->start_released = false;
        state_->staged = false;
        state_->generation_cancelled = false;
        ++state_->generation;
        state_->state = State::TeamState::command_active;
        state_->condition.notify_all();

        state_->condition.wait(lock, [&] {
            return state_->armed == state_->specs.size() || state_->team_stop.stop_requested();
        });
        if (state_->armed != state_->specs.size())
            return std::unexpected{worker_error("runtime.worker_team.invariant_failed",
                                                "worker_execute", "unexpected_worker_state")};
        state_->start_released = true;
        state_->condition.notify_all();
        state_->condition.wait(lock, [&] {
            return state_->completed == state_->specs.size() || state_->fatal_error.has_value();
        });
        if (state_->fatal_error)
            return std::unexpected{*state_->fatal_error};

        WorkerTeamCommandOutcome outcome;
        outcome.workers.reserve(state_->command_results.size());
        for (auto& slot : state_->command_results) {
            if (!slot) {
                state_->state = State::TeamState::fatal_failed;
                state_->team_stop.request_stop();
                return std::unexpected{worker_error("runtime.worker_team.invariant_failed",
                                                    "worker_execute", "unexpected_worker_state")};
            }
            outcome.workers.push_back(std::move(*slot));
        }
        state_->state = state_->team_stop.stop_requested() ? State::TeamState::stopping
                                                           : State::TeamState::ready;
        state_->operation = {};
        return outcome;
    } catch (const std::exception&) {
        auto error = contained_error(false, "worker_execute");
        {
            std::lock_guard lock{state_->mutex};
            if (state_->state == State::TeamState::command_active) {
                state_->fatal_error = error;
                state_->state = State::TeamState::fatal_failed;
                state_->team_stop.request_stop();
                state_->command_stop.request_stop();
            }
        }
        state_->condition.notify_all();
        return std::unexpected{std::move(error)};
    } catch (...) {
        auto error = contained_error(true, "worker_execute");
        {
            std::lock_guard lock{state_->mutex};
            if (state_->state == State::TeamState::command_active) {
                state_->fatal_error = error;
                state_->state = State::TeamState::fatal_failed;
                state_->team_stop.request_stop();
                state_->command_stop.request_stop();
            }
        }
        state_->condition.notify_all();
        return std::unexpected{std::move(error)};
    }
}

auto PersistentWorkerTeam::begin_generation(WorkerOperation operation,
                                            WorkerObservationPolicy observation,
                                            WorkerCancellationPolicy cancellation_policy)
    -> std::expected<PersistentWorkerGeneration, core::Error> {
    if (!state_)
        return std::unexpected{worker_error("runtime.worker_team.invalid_state",
                                            "worker_generation", "team_not_ready")};
    if (operation.run == nullptr)
        return std::unexpected{worker_error("runtime.worker_team.invalid_argument",
                                            "worker_generation", "missing_operation")};
    try {
        auto generation = std::make_unique<detail::PersistentWorkerGenerationState>();
        std::unique_lock lock{state_->mutex};
        if (state_->state != State::TeamState::ready)
            return std::unexpected{
                worker_error("runtime.worker_team.invalid_state", "worker_generation",
                             state_->state == State::TeamState::command_active ? "command_active"
                                                                               : "team_not_ready")};
        if (state_->generation == std::numeric_limits<std::uint64_t>::max()) {
            state_->state = State::TeamState::fatal_failed;
            state_->fatal_error = worker_error("runtime.worker_team.invariant_failed",
                                               "worker_generation", "generation_exhausted");
            state_->team_stop.request_stop();
            lock.unlock();
            state_->condition.notify_all();
            return std::unexpected{*state_->fatal_error};
        }

        state_->targeted = false;
        state_->targeted_control = nullptr;
        state_->operation = operation;
        state_->observation = observation;
        state_->cancellation_policy = cancellation_policy;
        state_->command_stop = std::stop_source{};
        std::ranges::fill(state_->command_results, std::nullopt);
        std::ranges::fill(state_->timed_signalled, false);
        std::ranges::fill(state_->pre_timed_errors, std::nullopt);
        state_->armed = 0;
        state_->timed_completed = 0;
        state_->completed = 0;
        state_->start_released = false;
        state_->staged = true;
        state_->generation_cancelled = false;
        ++state_->generation;
        generation->generation = state_->generation;
        for (std::size_t index = 0; index < state_->timed_signal_contexts.size(); ++index)
            state_->timed_signal_contexts[index] = {
                .state = state_.get(), .worker_index = index, .generation = state_->generation};
        state_->active_generation = generation.get();
        state_->state = State::TeamState::command_active;
        state_->condition.notify_all();
        return PersistentWorkerGeneration{state_.get(), std::move(generation)};
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "worker_generation")};
    } catch (...) {
        return std::unexpected{contained_error(true, "worker_generation")};
    }
}

PersistentWorkerGeneration::PersistentWorkerGeneration(
    void* team_state, std::unique_ptr<detail::PersistentWorkerGenerationState> state) noexcept
    : team_state_{team_state}, state_{std::move(state)} {}

PersistentWorkerGeneration::PersistentWorkerGeneration(PersistentWorkerGeneration&& other) noexcept
    : team_state_{std::exchange(other.team_state_, nullptr)}, state_{std::move(other.state_)} {}

PersistentWorkerGeneration::~PersistentWorkerGeneration() noexcept {
    if (state_ && !state_->terminal)
        std::terminate();
}

auto PersistentWorkerGeneration::await_ready() -> std::expected<void, core::Error> {
    if (!state_ || state_->terminal ||
        state_->phase != detail::PersistentWorkerGenerationState::Phase::arming)
        return std::unexpected{detail::generation_state_error("generation_invalid_phase")};
    auto& team = *static_cast<PersistentWorkerTeam::State*>(team_state_);
    std::unique_lock lock{team.mutex};
    if (team.active_generation != state_.get() || team.generation != state_->generation)
        return std::unexpected{detail::generation_state_error("generation_invalid_phase")};
    team.condition.wait(lock, [&] {
        return team.armed == team.specs.size() || team.fatal_error.has_value() ||
               (team.cancellation_policy == WorkerCancellationPolicy::observe &&
                team.cancellation.requested());
    });

    if (team.cancellation_policy == WorkerCancellationPolicy::observe &&
        team.cancellation.requested() && !team.fatal_error) {
        team.generation_cancelled = true;
        team.condition.notify_all();
        team.condition.wait(lock, [&] { return team.completed == team.specs.size(); });
        team.operation = {};
        team.staged = false;
        team.state = PersistentWorkerTeam::State::TeamState::ready;
        team.active_generation = nullptr;
        state_->phase = detail::PersistentWorkerGenerationState::Phase::terminal;
        state_->terminal = true;
        state_->terminal_error = detail::generation_cancelled_error();
        return std::unexpected{*state_->terminal_error};
    }
    if (team.fatal_error) {
        team.condition.wait(lock, [&] { return team.completed == team.specs.size(); });
        team.active_generation = nullptr;
        state_->phase = detail::PersistentWorkerGenerationState::Phase::terminal;
        state_->terminal = true;
        state_->terminal_error = *team.fatal_error;
        return std::unexpected{*state_->terminal_error};
    }
    state_->phase = detail::PersistentWorkerGenerationState::Phase::ready;
    return {};
}

void PersistentWorkerGeneration::cancel_before_release() noexcept {
    if (!state_ || state_->terminal ||
        state_->phase != detail::PersistentWorkerGenerationState::Phase::ready)
        std::terminate();
    auto& team = *static_cast<PersistentWorkerTeam::State*>(team_state_);
    std::unique_lock lock{team.mutex};
    if (team.active_generation != state_.get() || team.start_released)
        std::terminate();
    team.generation_cancelled = true;
    team.condition.notify_all();
    team.condition.wait(lock, [&] { return team.completed == team.specs.size(); });
    team.operation = {};
    team.staged = false;
    team.state = PersistentWorkerTeam::State::TeamState::ready;
    team.active_generation = nullptr;
    state_->phase = detail::PersistentWorkerGenerationState::Phase::terminal;
    state_->terminal = true;
}

void PersistentWorkerGeneration::release() noexcept {
    if (!state_ || state_->terminal ||
        state_->phase != detail::PersistentWorkerGenerationState::Phase::ready)
        std::terminate();
    auto& team = *static_cast<PersistentWorkerTeam::State*>(team_state_);
    std::lock_guard lock{team.mutex};
    if (team.active_generation != state_.get() || team.start_released)
        std::terminate();
    team.start_released = true;
    state_->phase = detail::PersistentWorkerGenerationState::Phase::released;
    team.condition.notify_all();
}

namespace {
auto lowest_error(const std::vector<std::optional<core::Error>>& errors)
    -> std::optional<core::Error> {
    for (const auto& error : errors)
        if (error)
            return *error;
    return std::nullopt;
}
} // namespace

auto PersistentWorkerGeneration::await_timed_completion() -> std::expected<void, core::Error> {
    if (!state_ || state_->terminal ||
        state_->phase != detail::PersistentWorkerGenerationState::Phase::released)
        return std::unexpected{detail::generation_state_error("generation_invalid_phase")};
    auto& team = *static_cast<PersistentWorkerTeam::State*>(team_state_);
    std::unique_lock lock{team.mutex};
    team.condition.wait(lock, [&] {
        return team.timed_completed == team.specs.size() || team.fatal_error.has_value() ||
               std::ranges::any_of(team.pre_timed_errors,
                                   [](const auto& error) { return error.has_value(); });
    });
    if (team.timed_completed == team.specs.size() && !team.fatal_error)
        return {};

    team.command_stop.request_stop();
    team.condition.notify_all();
    team.condition.wait(lock, [&] { return team.completed == team.specs.size(); });
    if (!team.fatal_error) {
        if (auto transfer_error =
                detail::PersistentWorkerTeamAccess::transfer_staged_outcome(&team, *state_)) {
            team.fatal_error = *transfer_error;
            team.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
            team.team_stop.request_stop();
        }
    }
    state_->phase = detail::PersistentWorkerGenerationState::Phase::terminal;
    state_->terminal = true;
    team.active_generation = nullptr;
    team.operation = {};
    team.staged = false;
    if (team.fatal_error) {
        state_->terminal_error = *team.fatal_error;
    } else {
        state_->terminal_error = lowest_error(team.pre_timed_errors);
        team.state = PersistentWorkerTeam::State::TeamState::ready;
    }
    return std::unexpected{*state_->terminal_error};
}

auto PersistentWorkerGeneration::await_quiescence() -> std::expected<void, core::Error> {
    if (!state_ || state_->terminal ||
        state_->phase != detail::PersistentWorkerGenerationState::Phase::released)
        return std::unexpected{detail::generation_state_error("generation_invalid_phase")};
    auto& team = *static_cast<PersistentWorkerTeam::State*>(team_state_);
    std::unique_lock lock{team.mutex};
    if (team.timed_completed != team.specs.size())
        return std::unexpected{detail::generation_state_error("generation_invalid_phase")};
    team.condition.wait(lock, [&] { return team.completed == team.specs.size(); });
    if (team.completed == team.specs.size() && !team.fatal_error) {
        if (auto transfer_error =
                detail::PersistentWorkerTeamAccess::transfer_staged_outcome(&team, *state_)) {
            team.fatal_error = *transfer_error;
            team.state = PersistentWorkerTeam::State::TeamState::fatal_failed;
            team.team_stop.request_stop();
        }
    }
    state_->phase = detail::PersistentWorkerGenerationState::Phase::terminal;
    state_->terminal = true;
    team.active_generation = nullptr;
    team.operation = {};
    team.staged = false;
    if (team.fatal_error) {
        state_->terminal_error = *team.fatal_error;
        return std::unexpected{*state_->terminal_error};
    }
    team.state = PersistentWorkerTeam::State::TeamState::ready;
    for (const auto& worker : state_->outcome->workers) {
        if (!worker.result) {
            state_->terminal_error = worker.result.error();
            return std::unexpected{*state_->terminal_error};
        }
    }
    return {};
}

auto PersistentWorkerGeneration::collect_ordered_results()
    -> std::expected<WorkerTeamCommandOutcome, core::Error> {
    if (!state_ || !state_->terminal)
        return std::unexpected{detail::generation_state_error("generation_invalid_phase")};
    if (state_->results_collected)
        return std::unexpected{
            detail::generation_state_error("generation_result_already_collected")};
    if (!state_->outcome)
        return std::unexpected{state_->terminal_error ? *state_->terminal_error
                                                      : detail::generation_state_error(
                                                            "generation_result_unavailable")};
    state_->results_collected = true;
    return std::move(*state_->outcome);
}

auto PersistentWorkerTeam::stop_and_join() -> std::expected<void, core::Error> {
    if (!state_)
        return {};
    require_injected_fault_script_owner(state_.get());
    {
        std::lock_guard lock{state_->mutex};
        if (state_->state == State::TeamState::joined)
            return {};
        state_->state = State::TeamState::stopping;
        if (state_->active_generation != nullptr) {
            state_->active_generation->phase =
                detail::PersistentWorkerGenerationState::Phase::terminal;
            state_->active_generation->terminal = true;
            state_->active_generation = nullptr;
        }
        if (state_->targeted_control != nullptr) {
            auto& control = *static_cast<detail::TargetedGenerationControl::State*>(
                state_->targeted_control);
            control.stop.request_stop();
            control.phase = detail::TargetedGenerationControl::State::Phase::terminal;
            control.results_collected = true;
        }
        state_->team_stop.request_stop();
        state_->command_stop.request_stop();
    }
    state_->condition.notify_all();

    const auto stop_exception = injected_monitoring_q1_stop_exception.load();
    if (stop_exception == 1U) {
        injected_monitoring_q1_stop_exception.store(0U);
        throw std::runtime_error{"injected Monitoring Q1 ordinary-stop failure"};
    }
    if (stop_exception == 2U) {
        injected_monitoring_q1_stop_exception.store(0U);
        throw 7;
    }

    std::optional<std::pair<std::size_t, core::Error>> first_error;
    for (std::size_t index = 0; index < state_->threads.size(); ++index) {
        auto& thread = state_->threads[index];
        thread.request_stop();
        if (!thread.joinable())
            continue;
        bool injected_stop{};
        for (std::size_t fault_index = 0; fault_index < injected_stop_worker_count.load();
             ++fault_index) {
            if (injected_stop_workers[fault_index] == static_cast<std::uint32_t>(index)) {
                injected_stop_workers[fault_index] = std::numeric_limits<std::uint32_t>::max();
                injected_stop = true;
                break;
            }
        }
        if (injected_stop) {
            if (!first_error)
                first_error.emplace(index, worker_error(
                    "runtime.worker_team.stop_failed", "worker_join", "join_failed"));
            continue;
        }
        try {
            thread.join();
        } catch (const std::system_error& error) {
            core::Error underlying =
                worker_error("runtime.worker_team.stop_failed", "worker_join", "join_failed");
            underlying.os_error = core::OsError::capture(error.code());
            if (!first_error)
                first_error.emplace(index, std::move(underlying));
        } catch (const std::exception&) {
            if (!first_error)
                first_error.emplace(index, contained_error(false, "worker_join"));
        } catch (...) {
            if (!first_error)
                first_error.emplace(index, contained_error(true, "worker_join"));
        }
    }
    const bool joinable =
        std::ranges::any_of(state_->threads, [](const auto& thread) { return thread.joinable(); });
    if (!joinable) {
        std::lock_guard lock{state_->mutex};
        state_->targeted_control = nullptr;
        state_->state = State::TeamState::joined;
    }
    if (!joinable && injected_inactive_stop_failure.exchange(false))
        return std::unexpected{
            worker_error("runtime.worker_team.stop_failed", "worker_join", "join_failed")};
    if (first_error) {
        if (joinable) {
            const auto eligibility_fault=injected_stop_eligibility_failure.exchange(0U);
            if(eligibility_fault==1U){
                for(auto& thread:state_->threads)if(thread.joinable())thread.join();
                std::lock_guard lock{state_->mutex};state_->targeted_control=nullptr;
                state_->state=State::TeamState::joined;
                return std::unexpected{contained_error(false,"worker_join")};
            }
            std::lock_guard lock{state_->mutex};
            if (!state_->team_stop.stop_requested() || !state_->command_stop.stop_requested() ||
                !std::ranges::any_of(state_->threads,
                                     [](const auto& thread) { return thread.joinable(); }))
                std::terminate();
            state_->force_eligible = true;
            if (stop_exception == 3U) {
                injected_monitoring_q1_stop_exception.store(0U);
                throw std::runtime_error{"injected Monitoring Q1 post-eligibility failure"};
            }
            if (stop_exception == 4U) {
                injected_monitoring_q1_stop_exception.store(0U);
                throw 7;
            }
            if(eligibility_fault==2U)
                return std::unexpected{contained_error(false,"worker_join")};
        }
        return std::unexpected{std::move(first_error->second)};
    }
    if (joinable) {
        std::lock_guard lock{state_->mutex};
        if (!state_->team_stop.stop_requested() || !state_->command_stop.stop_requested() ||
            !std::ranges::any_of(state_->threads,
                                 [](const auto& thread) { return thread.joinable(); }))
            std::terminate();
        state_->force_eligible = true;
        return std::unexpected{
            worker_error("runtime.worker_team.stop_failed", "worker_join", "join_failed")};
    }
    return {};
}

auto detail::PersistentWorkerTeamAccess::execute_cleanup_preallocated(
    PersistentWorkerTeam& team, CleanupWorkerOperation operation,
    std::span<CleanupWorkerSlot> slots) noexcept -> CleanupCommandResult {
    static_assert(std::is_nothrow_move_constructible_v<core::Error>);
    static_assert(std::is_nothrow_move_assignable_v<core::Error>);
    static_assert(std::is_nothrow_move_constructible_v<CleanupWorkerFailure>);
    static_assert(std::is_nothrow_move_assignable_v<CleanupWorkerFailure>);
    static_assert(std::is_nothrow_move_constructible_v<std::optional<core::Error>>);
    static_assert(std::is_nothrow_move_assignable_v<std::optional<core::Error>>);
    if (!team.state_ || operation.run == nullptr) std::terminate();
    auto& state = *team.state_;
    std::unique_lock lock{state.mutex};
    if (state.state != PersistentWorkerTeam::State::TeamState::ready ||
        state.active_generation != nullptr || state.cleanup_command ||
        slots.size() != state.specs.size()) std::terminate();
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].worker_id != state.specs[index].worker_id ||
            slots[index].disposition != CleanupWorkerDisposition::pristine ||
            slots[index].primary || slots[index].cleanup ||
            slots[index].owner_state_cookie_ != nullptr ||
            slots[index].self_address_cookie_ != nullptr ||
            slots[index].cleanup_generation_ != 0) std::terminate();
    }
    if (state.generation == std::numeric_limits<std::uint64_t>::max() ||
        state.cleanup_generation == std::numeric_limits<std::uint64_t>::max()) std::terminate();
    ++state.cleanup_generation;
    for (auto& slot : slots) {
        slot.owner_state_cookie_ = &state;
        slot.self_address_cookie_ = &slot;
        slot.cleanup_generation_ = state.cleanup_generation;
    }
    state.cleanup_operation = operation;
    state.cleanup_slots = slots;
    state.cleanup_command = true;
    state.armed = 0;
    state.completed = 0;
    state.start_released = false;
    state.generation_cancelled = false;
    state.staged = false;
    ++state.generation;
    state.state = PersistentWorkerTeam::State::TeamState::command_active;
    state.condition.notify_all();
    state.condition.wait(lock, [&] { return state.armed == state.specs.size(); });
    state.start_released = true;
    state.condition.notify_all();
    state.condition.wait(lock, [&] { return state.completed == state.specs.size(); });
    state.cleanup_command = false;
    state.cleanup_operation = {};
    state.state = PersistentWorkerTeam::State::TeamState::ready;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].owner_state_cookie_ != &state ||
            slots[index].self_address_cookie_ != &slots[index] ||
            slots[index].cleanup_generation_ != state.cleanup_generation ||
            slots[index].worker_id != state.specs[index].worker_id ||
            slots[index].disposition == CleanupWorkerDisposition::pristine)
            std::terminate();
    }
    state.cleanup_slots = {};
    return {.generation = CleanupGeneration{state.cleanup_generation}, .team_reusable = true};
}

void detail::PersistentWorkerTeamAccess::reset_cleanup_slots_after_consumption(
    PersistentWorkerTeam& team, CleanupGeneration generation,
    std::span<CleanupWorkerSlot> slots) noexcept {
    if (!team.state_) std::terminate();
    auto& state = *team.state_;
    std::lock_guard lock{state.mutex};
    if (generation.value() == 0 || generation.value() > state.cleanup_generation ||
        slots.size() != state.specs.size() || state.cleanup_command ||
        (state.state != PersistentWorkerTeam::State::TeamState::ready &&
         state.state != PersistentWorkerTeam::State::TeamState::joined)) std::terminate();
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto disposition = slots[index].disposition;
        const bool completed = disposition == CleanupWorkerDisposition::succeeded ||
                               disposition == CleanupWorkerDisposition::primary_failure ||
                               disposition == CleanupWorkerDisposition::contained_exception ||
                               disposition == CleanupWorkerDisposition::contained_unknown;
        if (!completed || slots[index].worker_id != state.specs[index].worker_id ||
            slots[index].primary || slots[index].cleanup ||
            slots[index].owner_state_cookie_ != &state ||
            slots[index].self_address_cookie_ != &slots[index] ||
            slots[index].cleanup_generation_ != generation.value()) std::terminate();
    }
    for (auto& slot : slots) {
        slot.disposition = CleanupWorkerDisposition::pristine;
        slot.owner_state_cookie_ = nullptr;
        slot.self_address_cookie_ = nullptr;
        slot.cleanup_generation_ = 0;
    }
}

auto detail::PersistentWorkerTeamAccess::stop_monitoring_q1_session_noexcept(
    PersistentWorkerTeam& team, core::Error standard, core::Error unknown) noexcept
    -> std::expected<void, core::Error> {
    const auto audit = [&](core::Error error) noexcept -> std::expected<void, core::Error> {
        if (!team.state_) return std::unexpected{std::move(error)};
        std::lock_guard lock{team.state_->mutex};
        const bool active = std::ranges::any_of(
            team.state_->threads, [](const auto& thread) { return thread.joinable(); });
        if (!active) {
            team.state_->targeted_control = nullptr;
            team.state_->state = PersistentWorkerTeam::State::TeamState::joined;
        } else {
            if (!team.state_->team_stop.stop_requested() ||
                !team.state_->command_stop.stop_requested()) std::terminate();
            team.state_->force_eligible = true;
        }
        return std::unexpected{std::move(error)};
    };
    try {
        return team.stop_and_join();
    } catch (const std::exception&) {
        return audit(std::move(standard));
    } catch (...) {
        return audit(std::move(unknown));
    }
}

auto detail::PersistentWorkerTeamAccess::force_stop_and_join_after_failure(
    PersistentWorkerTeam& team) noexcept -> ForcedTeamStopResult {
    if (!team.state_) std::terminate();
    auto& state = *team.state_;
    require_injected_fault_script_owner(&state);
    if (!state.force_eligible) std::terminate();
    if(force_interval_begin)force_interval_begin(force_interval_context);
    state.force_eligible = false;
    state.force_stop_error_count = 0;
    state.force_stop_error_capacity_exhausted = false;
    const auto normal_capacity = std::min(state.force_stop_errors.size() - 1U,
                                          injected_force_error_capacity.load());
    const auto record = [&](ForcedTeamStopError value) noexcept {
        if (state.force_stop_error_count < normal_capacity) {
            state.force_stop_errors[state.force_stop_error_count++] = value;
            return;
        }
        if (!state.force_stop_error_capacity_exhausted) {
            state.force_stop_error_capacity_exhausted = true;
            state.force_stop_errors[normal_capacity] = {
                ForcedTeamStopErrorKind::capacity_exhausted, value.sweep, 0};
            state.force_stop_error_count = normal_capacity + 1U;
        }
    };
    const auto any_joinable = [&]() noexcept {
        return std::ranges::any_of(state.threads,
                                   [](const auto& thread) { return thread.joinable(); });
    };
    if (!any_joinable()) std::terminate();
    std::uint8_t attempted{};
    for (std::uint8_t sweep = 1; sweep <= 2 && any_joinable(); ++sweep) {
        attempted = sweep;
        bool contained{};
        for (std::size_t index = 0; index < state.threads.size(); ++index) {
            auto& thread = state.threads[index];
            if (!thread.joinable()) continue;
            thread.request_stop();
            auto fault = ForcedTeamStopTestFault::none;
            for (std::size_t fault_index = 0; fault_index < injected_force_fault_count.load();
                 ++fault_index) {
                auto& candidate = injected_force_faults[fault_index];
                if (!candidate.consumed && candidate.sweep == sweep &&
                    candidate.worker == index) {
                    candidate.consumed = true;
                    fault = candidate.kind;
                    break;
                }
            }
            if (fault != ForcedTeamStopTestFault::none) {
                if (fault == ForcedTeamStopTestFault::join_failed) {
                    record({ForcedTeamStopErrorKind::join_failed, sweep,
                            static_cast<std::uint32_t>(index)});
                    continue;
                }
                record({fault == ForcedTeamStopTestFault::contained_exception
                            ? ForcedTeamStopErrorKind::contained_exception
                            : ForcedTeamStopErrorKind::contained_unknown_failure,
                        sweep, 0});
                contained = true;
                break;
            }
            try {
                thread.join();
            } catch (const std::system_error&) {
                record({ForcedTeamStopErrorKind::join_failed, sweep,
                        static_cast<std::uint32_t>(index)});
            } catch (const std::exception&) {
                record({ForcedTeamStopErrorKind::contained_exception, sweep, 0});
                contained = true;
                break;
            } catch (...) {
                record({ForcedTeamStopErrorKind::contained_unknown_failure, sweep, 0});
                contained = true;
                break;
            }
        }
        if (contained) continue;
    }
    const bool active = any_joinable();
    if (!active) {
        // Every worker is joined and no worker can observe State again; this enum store is the
        // mechanically nonthrowing terminal transition required by the private force contract.
        state.state = PersistentWorkerTeam::State::TeamState::joined;
        state.targeted_control = nullptr;
        if(force_interval_end)force_interval_end(force_interval_context);
    }
    return {.disposition = active ? ForcedTeamStopDisposition::active
                                  : ForcedTeamStopDisposition::inactive,
            .sweeps_attempted = attempted,
            .errors = std::span<const ForcedTeamStopError>{state.force_stop_errors.data(),
                                                           state.force_stop_error_count},
            .error_capacity_exhausted = state.force_stop_error_capacity_exhausted};
}

void detail::PersistentWorkerTeamAccess::set_force_interval_hooks_for_test(
 void* context,void (*begin)(void*) noexcept,void (*end)(void*) noexcept) noexcept{
 force_interval_context=context;force_interval_begin=begin;force_interval_end=end;
}

void detail::PersistentWorkerTeamAccess::inject_stop_failure_for_test(
    PersistentWorkerTeam& team, std::uint32_t worker_index) noexcept {
    if (!team.state_) std::terminate();
    acquire_injected_fault_script(team.state_.get());
    if (worker_index >= team.state_->specs.size()) std::terminate();
    const auto index = injected_stop_worker_count.load();
    if (index >= injected_stop_workers.size()) std::terminate();
    for (std::size_t existing = 0; existing < index; ++existing)
        if (injected_stop_workers[existing] == worker_index) std::terminate();
    injected_stop_workers[index] = worker_index;
    injected_stop_worker_count.store(index + 1U);
}
void detail::PersistentWorkerTeamAccess::inject_inactive_stop_failure_for_test(
    PersistentWorkerTeam& team) noexcept {
    if (!team.state_) std::terminate();
    acquire_injected_fault_script(team.state_.get());
    injected_inactive_stop_failure.store(true);
}
void detail::PersistentWorkerTeamAccess::inject_stop_eligibility_failure_for_test(
 PersistentWorkerTeam& team,StopEligibilityTestFault fault) noexcept{
 if(!team.state_) std::terminate();
 acquire_injected_fault_script(team.state_.get());
 injected_stop_eligibility_failure.store(fault==StopEligibilityTestFault::before_publication?1U:2U);
}
void detail::PersistentWorkerTeamAccess::inject_monitoring_q1_stop_exception_for_test(
    PersistentWorkerTeam& team, MonitoringQ1StopExceptionTestFault fault) noexcept {
    if (!team.state_) std::terminate();
    acquire_injected_fault_script(team.state_.get());
    injected_monitoring_q1_stop_exception.store(static_cast<unsigned>(fault) + 1U);
}

void detail::PersistentWorkerTeamAccess::inject_force_stop_failure_for_test(
    PersistentWorkerTeam& team, std::uint8_t sweep, std::uint32_t worker_index,
    ForcedTeamStopTestFault kind) noexcept {
    if (!team.state_) std::terminate();
    acquire_injected_fault_script(team.state_.get());
    if (sweep == 0 || sweep > 2 || worker_index >= team.state_->specs.size() ||
        kind == ForcedTeamStopTestFault::none) std::terminate();
    const auto index = injected_force_fault_count.load();
    if (index >= injected_force_faults.size()) std::terminate();
    for (std::size_t existing = 0; existing < index; ++existing)
        if (injected_force_faults[existing].sweep == sweep &&
            injected_force_faults[existing].worker == worker_index)
            std::terminate();
    injected_force_faults[index] = {
        .sweep = sweep, .worker = worker_index, .kind = kind, .consumed = false};
    injected_force_fault_count.store(index + 1U);
}

void detail::PersistentWorkerTeamAccess::set_force_error_capacity_for_test(
    PersistentWorkerTeam& team, std::size_t capacity) noexcept {
    if (!team.state_) std::terminate();
    acquire_injected_fault_script(team.state_.get());
    injected_force_error_capacity.store(capacity);
}

void detail::PersistentWorkerTeamAccess::clear_stop_failures_for_test(
    PersistentWorkerTeam& team) noexcept {
    if (!team.state_) std::terminate();
    require_injected_fault_script_owner(team.state_.get());
    injected_stop_worker_count.store(0);
    injected_inactive_stop_failure.store(false);
    injected_stop_eligibility_failure.store(0U);
    injected_monitoring_q1_stop_exception.store(0U);
    injected_force_fault_count.store(0);
    injected_force_error_capacity.store(std::numeric_limits<std::size_t>::max());
    if (owns_injected_fault_script) {
        injected_fault_script_owner.store(nullptr);
        owns_injected_fault_script = false;
        injected_fault_script_guard.clear();
    }
}

auto PersistentWorkerTeam::ready() const noexcept -> bool {
    if (!state_)
        return false;
    std::lock_guard lock{state_->mutex};
    return state_->state == State::TeamState::ready;
}

auto PersistentWorkerTeam::active() const noexcept -> bool {
    if (!state_)
        return false;
    std::lock_guard lock{state_->mutex};
    return state_->state != State::TeamState::joined;
}

detail::TargetedGenerationControl::TargetedGenerationControl(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
detail::TargetedGenerationControl::TargetedGenerationControl(TargetedGenerationControl&&) noexcept =
    default;
detail::TargetedGenerationControl::~TargetedGenerationControl() noexcept {
    if (state_ && (state_->borrowed ||
                   (state_->phase != State::Phase::pristine && !state_->stop.stop_requested())))
        std::terminate();
}

auto detail::PersistentWorkerTeamAccess::make_targeted_control()
    -> std::expected<TargetedGenerationControl, core::Error> {
    try {
        return TargetedGenerationControl{std::make_unique<TargetedGenerationControl::State>()};
    } catch (const std::exception&) {
        return std::unexpected{contained_error(false, "worker_generation")};
    } catch (...) {
        return std::unexpected{contained_error(true, "worker_generation")};
    }
}

void detail::PersistentWorkerTeamAccess::corrupt_targeted_snapshot_for_test(
    PersistentWorkerTeam& team, TargetedSnapshotTestFault fault) {
    std::lock_guard lock{team.state_->mutex};
    auto& s = team.state_->specs;
    auto& r = team.state_->readiness;
    switch (fault) {
    case TargetedSnapshotTestFault::spec_index: ++s[0].worker_index; break;
    case TargetedSnapshotTestFault::spec_worker_id: std::swap(s[0].worker_id, s[1].worker_id); break;
    case TargetedSnapshotTestFault::spec_processing_unit: std::swap(s[0].processing_unit, s[1].processing_unit); break;
    case TargetedSnapshotTestFault::spec_core: std::swap(s[0].core, s[1].core); break;
    case TargetedSnapshotTestFault::spec_stable_pu: std::swap(s[0].stable_pu, s[1].stable_pu); break;
    case TargetedSnapshotTestFault::spec_stable_core: std::swap(s[0].stable_core, s[1].stable_core); break;
    case TargetedSnapshotTestFault::spec_cpu: std::swap(s[0].requested_cpu, s[1].requested_cpu); break;
    case TargetedSnapshotTestFault::ready_index: ++r[0].worker_index; break;
    case TargetedSnapshotTestFault::ready_worker_id: std::swap(r[0].worker_id, r[1].worker_id); break;
    case TargetedSnapshotTestFault::ready_stable_pu: std::swap(r[0].stable_pu, r[1].stable_pu); break;
    case TargetedSnapshotTestFault::ready_stable_core: std::swap(r[0].stable_core, r[1].stable_core); break;
    case TargetedSnapshotTestFault::ready_cpu: std::swap(r[0].requested_cpu, r[1].requested_cpu); break;
    case TargetedSnapshotTestFault::ready_affinity:
        r[0].observed_affinity = core::CpuSet{s[1].requested_cpu}; break;
    case TargetedSnapshotTestFault::ready_observed_cpu:
        r[0].observed_cpu = s[1].requested_cpu; break;
    }
}

auto detail::PersistentWorkerTeamAccess::begin_targeted_generation_preallocated(
    const AdmittedPlacementRequest& admitted, PersistentWorkerTeam& team,
    TargetedGenerationControl& control, std::size_t target, TargetedWorkerOperation operation,
    WorkerCancellationPolicy cancellation_policy)
    -> std::expected<TargetedWorkerGeneration, core::Error> {
    auto projected = project(admitted.workers());
    if (!projected)
        return std::unexpected{std::move(projected.error())};
    return begin_targeted_generation_preallocated(*projected, team, control, target,
                                                  operation, cancellation_policy);
}

auto detail::PersistentWorkerTeamAccess::begin_q2_targeted_generation_preallocated(
    std::span<const WorkerLaunchSpec> expected, PersistentWorkerTeam& team,
    TargetedGenerationControl& control, std::size_t target, TargetedWorkerOperation operation,
    TargetedWorkerReadinessOperation readiness,
    WorkerCancellationPolicy cancellation_policy)
    -> std::expected<TargetedWorkerGeneration, core::Error> {
    const auto invalid = [](std::string classification) {
        return worker_error("runtime.worker_team.invalid_argument", "worker_generation",
                            std::move(classification));
    };
    if (!control.state_ || control.state_->borrowed ||
        control.state_->phase != TargetedGenerationControl::State::Phase::pristine)
        return std::unexpected{generation_state_error("targeted_control_invalid")};
    if (!team.state_)
        return std::unexpected{generation_state_error("team_not_ready")};
    std::lock_guard lock{team.state_->mutex};
    if (team.state_->state == PersistentWorkerTeam::State::TeamState::command_active)
        return std::unexpected{generation_state_error("command_active")};
    if (team.state_->state != PersistentWorkerTeam::State::TeamState::ready)
        return std::unexpected{generation_state_error("team_not_ready")};
    if (!operation.run)
        return std::unexpected{invalid("missing_operation")};
    if (target >= expected.size())
        return std::unexpected{invalid("target_index_out_of_range")};
    if (expected.size() != team.state_->specs.size() ||
        team.state_->readiness.size() != team.state_->specs.size())
        return std::unexpected{invalid("target_worker_mismatch")};
    for (std::size_t i = 0; i < team.state_->specs.size(); ++i) {
        const auto& a = expected[i];
        const auto& s = team.state_->specs[i];
        const auto& r = team.state_->readiness[i];
        if (s.worker_index != i || s.worker_id != core::WorkerId{static_cast<std::uint32_t>(i)} ||
            s.processing_unit != a.processing_unit || s.core != a.core ||
            s.stable_pu != a.stable_pu || s.stable_core != a.stable_core ||
            s.requested_cpu != a.requested_cpu || r.worker_index != i ||
            r.worker_id != s.worker_id || r.stable_pu != s.stable_pu ||
            r.stable_core != s.stable_core || r.requested_cpu != s.requested_cpu ||
            r.observed_affinity != core::CpuSet{s.requested_cpu} ||
            r.observed_cpu != s.requested_cpu)
            return std::unexpected{invalid("target_worker_mismatch")};
    }
    if (team.state_->generation == std::numeric_limits<std::uint64_t>::max()) {
        team.state_->state = PersistentWorkerTeam::State::TeamState::fatal_failed;
        team.state_->fatal_error = worker_error("runtime.worker_team.invariant_failed",
                                                "worker_generation", "generation_exhausted");
        team.state_->team_stop.request_stop();
        team.state_->condition.notify_all();
        return std::unexpected{*team.state_->fatal_error};
    }
    if (targeted_publication_hook)
        targeted_publication_hook(targeted_publication_hook_context);
    auto& c = *control.state_;
    c.borrowed = true;
    c.phase = TargetedGenerationControl::State::Phase::arming;
    c.results_collected = false;
    c.result.reset();
    c.terminal_error.reset();
    c.disposition = TargetedGenerationControl::State::Disposition::pristine;
    c.opaque_token = 0;
    c.cancellation_observed = false;
    team.state_->operation = {};
    team.state_->targeted_operation = operation;
    team.state_->targeted_readiness = readiness;
    team.state_->observation = WorkerObservationPolicy::none;
    team.state_->cancellation_policy = cancellation_policy;
    std::ranges::fill(team.state_->command_results, std::nullopt);
    std::ranges::fill(team.state_->timed_signalled, false);
    std::ranges::fill(team.state_->pre_timed_errors, std::nullopt);
    team.state_->armed = 0;
    team.state_->timed_completed = 0;
    team.state_->completed = 0;
    team.state_->start_released = false;
    team.state_->staged = true;
    team.state_->generation_cancelled = false;
    team.state_->targeted = true;
    team.state_->targeted_index = target;
    team.state_->targeted_control = &c;
    ++team.state_->generation;
    c.generation = team.state_->generation;
    team.state_->timed_signal_contexts[target] = {team.state_.get(), target,
                                                  team.state_->generation};
    team.state_->state = PersistentWorkerTeam::State::TeamState::command_active;
    team.state_->condition.notify_all();
    return TargetedWorkerGeneration{team.state_.get(), &c};
}

auto detail::PersistentWorkerTeamAccess::begin_targeted_generation_preallocated(
    std::span<const WorkerLaunchSpec> expected, PersistentWorkerTeam& team,
    TargetedGenerationControl& control, std::size_t target, TargetedWorkerOperation operation,
    WorkerCancellationPolicy cancellation_policy)
    -> std::expected<TargetedWorkerGeneration, core::Error> {
    return begin_q2_targeted_generation_preallocated(expected, team, control, target, operation,
                                                      {}, cancellation_policy);
}

detail::TargetedWorkerGeneration::TargetedWorkerGeneration(
    void* team, TargetedGenerationControl::State* state) noexcept
    : team_{team}, state_{state} {}
detail::TargetedWorkerGeneration::TargetedWorkerGeneration(
    TargetedWorkerGeneration&& other) noexcept
    : team_{std::exchange(other.team_, nullptr)}, state_{std::exchange(other.state_, nullptr)} {}
detail::TargetedWorkerGeneration::~TargetedWorkerGeneration() noexcept {
    if (state_ && state_->phase != TargetedGenerationControl::State::Phase::terminal)
        std::terminate();
    if (state_ && state_->results_collected) {
        state_->borrowed = false;
    }
}

auto detail::PersistentWorkerTeamAccess::reset_targeted_control(
    TargetedGenerationControl& control) noexcept -> std::expected<void, core::Error> {
    if (!control.state_ || control.state_->borrowed ||
        control.state_->phase != TargetedGenerationControl::State::Phase::terminal ||
        !control.state_->results_collected || control.state_->stop.stop_requested())
        return std::unexpected{generation_state_error("targeted_control_reset_invalid")};
    control.state_->generation = 0;
    control.state_->results_collected = false;
    control.state_->result.reset();
    control.state_->terminal_error.reset();
    control.state_->opaque_token = 0;
    control.state_->cancellation_observed = false;
    control.state_->disposition = TargetedGenerationControl::State::Disposition::pristine;
    control.state_->phase = TargetedGenerationControl::State::Phase::pristine;
    return {};
}

auto detail::TargetedWorkerGeneration::await_ready() -> std::expected<void, core::Error> {
    if (!state_ || state_->phase != TargetedGenerationControl::State::Phase::arming)
        return std::unexpected{generation_state_error("generation_invalid_phase")};
    auto& t = *static_cast<PersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{t.mutex};
    t.condition.wait(lock, [&] {
        return t.armed == 1 || t.fatal_error.has_value() ||
               (t.cancellation_policy == WorkerCancellationPolicy::observe &&
                t.cancellation.requested());
    });
    if (t.cancellation_policy == WorkerCancellationPolicy::observe && t.cancellation.requested() &&
        !t.fatal_error) {
        t.generation_cancelled = true;
        t.condition.notify_all();
        t.condition.wait(lock, [&] { return t.completed == 1; });
        t.operation = {};
        t.staged = false;
        t.targeted = false;
        t.targeted_control = nullptr;
        t.state = PersistentWorkerTeam::State::TeamState::ready;
        state_->phase = TargetedGenerationControl::State::Phase::terminal;
        state_->results_collected = true;
        state_->terminal_error = generation_cancelled_error();
        return std::unexpected{*state_->terminal_error};
    }
    if (t.fatal_error) {
        state_->phase = TargetedGenerationControl::State::Phase::terminal;
        state_->results_collected = true;
        state_->terminal_error = *t.fatal_error;
        return std::unexpected{*state_->terminal_error};
    }
    if (state_->terminal_error) {
        t.condition.wait(lock, [&] { return t.completed == 1; });
        t.operation = {};
        t.targeted_operation = {};
        t.targeted_readiness = {};
        t.staged = false;
        t.targeted = false;
        t.targeted_control = nullptr;
        t.state = PersistentWorkerTeam::State::TeamState::ready;
        state_->phase = TargetedGenerationControl::State::Phase::terminal;
        state_->results_collected = true;
        return std::unexpected{*state_->terminal_error};
    }
    state_->phase = TargetedGenerationControl::State::Phase::ready;
    return {};
}
void detail::TargetedWorkerGeneration::cancel_before_release() noexcept {
    if (!state_ || state_->phase != TargetedGenerationControl::State::Phase::ready)
        std::terminate();
    auto& t = *static_cast<PersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{t.mutex};
    t.generation_cancelled = true;
    t.condition.notify_all();
    t.condition.wait(lock, [&] { return t.completed == 1; });
    t.operation = {};
    t.staged = false;
    t.targeted = false;
    t.targeted_control = nullptr;
    t.state = PersistentWorkerTeam::State::TeamState::ready;
    state_->phase = TargetedGenerationControl::State::Phase::terminal;
    state_->results_collected = true;
}
void detail::TargetedWorkerGeneration::release() noexcept {
    if (!state_ || state_->phase != TargetedGenerationControl::State::Phase::ready)
        std::terminate();
    auto& t = *static_cast<PersistentWorkerTeam::State*>(team_);
    std::lock_guard lock{t.mutex};
    t.start_released = true;
    state_->phase = TargetedGenerationControl::State::Phase::released;
    t.condition.notify_all();
}
auto detail::TargetedWorkerGeneration::await_timed_completion()
    -> std::expected<void, core::Error> {
    if (!state_ || state_->phase != TargetedGenerationControl::State::Phase::released)
        return std::unexpected{generation_state_error("generation_invalid_phase")};
    auto& t = *static_cast<PersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{t.mutex};
    t.condition.wait(lock, [&] {
        return t.timed_completed == 1 || t.completed == 1 || t.fatal_error ||
               t.pre_timed_errors[t.targeted_index];
    });
    if (t.timed_completed == 1 && !t.fatal_error)
        return {};
    if (t.completed == 1 && !t.fatal_error) {
        t.operation = {};
        t.staged = false;
        t.targeted = false;
        t.targeted_control = nullptr;
        state_->phase = TargetedGenerationControl::State::Phase::terminal;
        auto disposition = TargetedWorkerDisposition::invalid_callback_result;
        std::string_view classification{"unexpected_worker_state"};
        using Disposition = TargetedGenerationControl::State::Disposition;
        switch (state_->disposition) {
        case Disposition::operation_failed:
            disposition = TargetedWorkerDisposition::caller_failure;
            classification = "worker_operation_failed";
            break;
        case Disposition::standard_exception:
            disposition = TargetedWorkerDisposition::standard_exception;
            classification = "contained_exception";
            break;
        case Disposition::unknown_exception:
            disposition = TargetedWorkerDisposition::unknown_exception;
            classification = "contained_unknown_failure";
            break;
        case Disposition::runner_cancelled:
            disposition = TargetedWorkerDisposition::runner_cancelled;
            classification = "runner_cancelled";
            break;
        case Disposition::team_stopped:
            disposition = TargetedWorkerDisposition::team_stopped;
            classification = "team_stopped";
            break;
        case Disposition::missing_signal:
            disposition = TargetedWorkerDisposition::missing_signal;
            classification = "missing_timed_completion_signal";
            break;
        case Disposition::signal_before_release:
            disposition = TargetedWorkerDisposition::signal_before_release;
            classification = "timed_signal_before_release";
            break;
        case Disposition::invalid_callback_result:
            disposition = TargetedWorkerDisposition::invalid_callback_result;
            classification = "invalid_callback_result";
            break;
        default: break;
        }
        state_->result.emplace(TargetedWorkerResult{t.specs[t.targeted_index].worker_id,
                                                    disposition, state_->opaque_token,
                                                    state_->cancellation_observed});
        if (disposition != TargetedWorkerDisposition::missing_signal &&
            disposition != TargetedWorkerDisposition::signal_before_release &&
            disposition != TargetedWorkerDisposition::invalid_callback_result)
            t.state = PersistentWorkerTeam::State::TeamState::ready;
        const bool invariant = disposition == TargetedWorkerDisposition::missing_signal ||
                               disposition == TargetedWorkerDisposition::signal_before_release ||
                               disposition == TargetedWorkerDisposition::invalid_callback_result;
        state_->terminal_error = worker_error(
            invariant ? "runtime.worker_team.invariant_failed"
                      : "runtime.worker_team.command_failed",
            "worker_generation", std::string{classification});
        return std::unexpected{*state_->terminal_error};
    }
    t.condition.wait(lock, [&] { return t.completed == 1; });
    state_->phase = TargetedGenerationControl::State::Phase::terminal;
    state_->results_collected = true;
    t.operation = {};
    t.staged = false;
    t.targeted = false;
    t.targeted_control = nullptr;
    if (t.fatal_error)
        state_->terminal_error = *t.fatal_error;
    else {
        state_->terminal_error = t.pre_timed_errors[t.targeted_index];
        t.state = PersistentWorkerTeam::State::TeamState::ready;
    }
    return std::unexpected{*state_->terminal_error};
}
auto detail::TargetedWorkerGeneration::await_quiescence() -> std::expected<void, core::Error> {
    if (!state_ || state_->phase != TargetedGenerationControl::State::Phase::released)
        return std::unexpected{generation_state_error("generation_invalid_phase")};
    auto& t = *static_cast<PersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{t.mutex};
    if (t.timed_completed != 1 && t.completed != 1)
        return std::unexpected{generation_state_error("generation_invalid_phase")};
    t.condition.wait(lock, [&] { return t.completed == 1; });
    state_->phase = TargetedGenerationControl::State::Phase::terminal;
    t.operation = {};
    t.staged = false;
    t.targeted = false;
    t.targeted_control = nullptr;
    using Disposition = TargetedGenerationControl::State::Disposition;
    auto disposition = TargetedWorkerDisposition::invalid_callback_result;
    switch (state_->disposition) {
    case Disposition::succeeded: disposition = TargetedWorkerDisposition::succeeded; break;
    case Disposition::operation_failed: disposition = TargetedWorkerDisposition::caller_failure; break;
    case Disposition::standard_exception: disposition = TargetedWorkerDisposition::standard_exception; break;
    case Disposition::unknown_exception: disposition = TargetedWorkerDisposition::unknown_exception; break;
    case Disposition::runner_cancelled: disposition = TargetedWorkerDisposition::runner_cancelled; break;
    case Disposition::team_stopped: disposition = TargetedWorkerDisposition::team_stopped; break;
    case Disposition::missing_signal: disposition = TargetedWorkerDisposition::missing_signal; break;
    case Disposition::duplicate_signal: disposition = TargetedWorkerDisposition::duplicate_signal; break;
    case Disposition::signal_before_release:
        disposition = TargetedWorkerDisposition::signal_before_release; break;
    case Disposition::pristine:
    case Disposition::invalid_callback_result: break;
    }
    state_->result.emplace(TargetedWorkerResult{
        .worker_id = t.specs[t.targeted_index].worker_id,
        .disposition = disposition,
        .opaque_token = state_->opaque_token,
        .cancellation_observed = state_->cancellation_observed});
    if (t.fatal_error) {
        state_->terminal_error = *t.fatal_error;
        return std::unexpected{*state_->terminal_error};
    }
    const bool permanently_fatal =
        disposition == TargetedWorkerDisposition::missing_signal ||
        disposition == TargetedWorkerDisposition::duplicate_signal ||
        disposition == TargetedWorkerDisposition::signal_before_release ||
        disposition == TargetedWorkerDisposition::invalid_callback_result;
    if (!permanently_fatal)
        t.state = PersistentWorkerTeam::State::TeamState::ready;
    else {
        const std::string_view classification =
            disposition == TargetedWorkerDisposition::missing_signal
                ? "missing_timed_completion_signal"
                : disposition == TargetedWorkerDisposition::duplicate_signal
                      ? "duplicate_timed_completion_signal"
                      : disposition == TargetedWorkerDisposition::signal_before_release
                            ? "timed_signal_before_release"
                            : "invalid_callback_result";
        state_->terminal_error = worker_error("runtime.worker_team.invariant_failed",
                                              "worker_generation",
                                              std::string{classification});
        return std::unexpected{*state_->terminal_error};
    }
    return {};
}
auto detail::TargetedWorkerGeneration::collect_result()
    -> std::expected<TargetedWorkerResult, core::Error> {
    if (!state_ || state_->phase != TargetedGenerationControl::State::Phase::terminal)
        return std::unexpected{generation_state_error("generation_invalid_phase")};
    if (state_->results_collected)
        return std::unexpected{generation_state_error("generation_result_already_collected")};
    if (!state_->result)
    {
        state_->results_collected = true;
        return std::unexpected{state_->terminal_error
                                   ? *state_->terminal_error
                                   : generation_state_error("generation_result_unavailable")};
    }
    state_->results_collected = true;
    return std::move(*state_->result);
}

} // namespace epycbench::runtime
