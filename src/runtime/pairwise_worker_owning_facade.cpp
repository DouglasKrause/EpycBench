#include "runtime/pairwise_worker_owning_facade.hpp"

#include "epycbench/runtime/pairwise_finalized_error_profile.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <ranges>
#include <system_error>
#include <type_traits>

namespace epycbench::runtime::detail {

struct PairwiseWorkerReadinessBorrowState final {
    void* team{};
    std::uint64_t generation{};
    bool outstanding{};
};

namespace {

[[nodiscard]] auto worker_failure(
    PairwiseFailureCode code, PairwiseFailureOperation operation,
    PairwiseFailureReason reason, PairwiseFailureContext context,
    std::optional<int> error_number = std::nullopt) noexcept -> PairwiseWorkerFailure {
    auto value = PairwiseWorkerFailure::create(code, operation, reason,
                                               std::move(context), error_number);
    if (!value)
        std::terminate();
    return std::move(*value);
}

[[nodiscard]] auto phase(PairwiseLifecyclePhase value) noexcept
    -> PairwiseFailureContext {
    return LifecyclePhaseContext{value};
}

[[nodiscard]] auto runtime_preparation_failure() noexcept -> PairwiseRuntimeFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::contained_exception,
        LifecyclePhaseContext{PairwiseLifecyclePhase::preparation});
    if (!value)
        std::terminate();
    return std::move(*value);
}

template <class Failure>
[[nodiscard]] auto nested_context(const Failure& failure) noexcept
    -> PairwiseFailureContext {
    return std::visit(
        [&failure]<class Context>(const Context& context) -> PairwiseFailureContext {
            if constexpr (std::is_same_v<Context, NestedFailureContext>) {
                auto platform = context.nested_platform_failure;
                if (!platform &&
                    pairwise_failure_subsystem(context.failure.code) ==
                        PairwiseFailureSubsystem::platform)
                    platform = context.failure;
                return NestedFailureContext{
                    PairwiseNestedFailureValue{
                        failure.code(), failure.operation(), failure.reason(),
                        NoPairwiseFailureContext{}, failure.posix_errno()},
                    std::nullopt, platform};
            } else {
                return NestedFailureContext{
                    PairwiseNestedFailureValue{failure.code(), failure.operation(),
                                               failure.reason(),
                                               PairwiseFlatFailureContext{context},
                                               failure.posix_errno()},
                    std::nullopt, std::nullopt};
            }
        },
        failure.context());
}

[[nodiscard]] auto nested_context(const PairwiseEvidenceFailure& failure) noexcept
    -> PairwiseFailureContext {
    return std::visit([](const auto& value) noexcept {
        return nested_context(value);
    }, failure);
}

[[nodiscard]] auto worker_operation_context(
    const PairwiseEvidenceFailure& failure) noexcept -> PairwiseFailureContext {
    auto context = nested_context(failure);
    std::get<NestedFailureContext>(context).phase =
        PairwiseLifecyclePhase::worker_callback;
    return context;
}

[[nodiscard]] auto placement_failure(PairwiseFailureReason reason,
                                     PairwiseFailureContext context) noexcept
    -> PairwiseWorkerFailure {
    return worker_failure(PairwiseFailureCode::worker_placement_failed,
                          PairwiseFailureOperation::worker_startup, reason,
                          std::move(context));
}

[[nodiscard]] auto command_failure(PairwiseFailureReason reason,
                                   PairwiseFailureContext context) noexcept
    -> PairwiseWorkerFailure {
    return worker_failure(PairwiseFailureCode::worker_command_failed,
                          PairwiseFailureOperation::worker_command, reason,
                          std::move(context));
}

[[nodiscard]] auto contained(PairwiseFailureOperation operation,
                             PairwiseLifecyclePhase lifecycle,
                             bool unknown = false) noexcept -> PairwiseWorkerFailure {
    return worker_failure(
        unknown ? PairwiseFailureCode::worker_contained_unknown_failure
                : PairwiseFailureCode::worker_contained_exception,
        operation,
        unknown ? PairwiseFailureReason::contained_unknown_failure
                : PairwiseFailureReason::contained_exception,
        phase(lifecycle));
}

[[nodiscard]] auto is_worker_lower(const PairwiseEvidenceFailure& failure) noexcept -> bool {
    return pairwise_context_satisfies_constraint(
        PairwiseContextConstraint::nested_worker_operation_failure,
        PairwiseFailureReason::worker_operation_failed,
        worker_operation_context(failure));
}

[[nodiscard]] auto checked_capacity(std::uint64_t warmup,
                                    std::uint64_t blocks) noexcept
    -> std::optional<std::size_t> {
    constexpr std::uint64_t fixed{132U};
    if (warmup > (std::numeric_limits<std::uint64_t>::max() - fixed) / 2U)
        return std::nullopt;
    const auto with_warmup = fixed + warmup * 2U;
    if (blocks > (std::numeric_limits<std::uint64_t>::max() - with_warmup) / 4U)
        return std::nullopt;
    const auto total = with_warmup + blocks * 4U;
    if (total > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return static_cast<std::size_t>(total);
}

[[nodiscard]] auto production_launch(void*, void (*entry)(void*, std::size_t) noexcept,
                                     void* state, std::size_t index) -> std::jthread {
    return std::jthread{entry, state, index};
}

void before(const PairwiseWorkerOwningHooks& hooks,
            PairwiseWorkerOwningOperation operation, std::size_t index) {
    if (hooks.before_operation != nullptr)
        hooks.before_operation(hooks.context, operation, index);
}

} // namespace

auto PairwiseWorkerOperationFailure::create(PairwiseEvidenceFailure failure) noexcept
    -> std::optional<PairwiseWorkerOperationFailure> {
    if (!is_worker_lower(failure))
        return std::nullopt;
    return PairwiseWorkerOperationFailure{std::move(failure)};
}

struct PairwiseWorkerCommandOutcome::State {
    enum class Phase : std::uint8_t { prepared, arming, ready, released, terminal, collected };

    State(PairwiseWorkerGenerationClass value,
          std::span<const WorkerLaunchSpec> workers)
        : generation_class{value}, timed(workers.size(), 0U),
          pre_timed_failures(workers.size()), signal_contexts(workers.size()) {
        results.reserve(workers.size());
        for (const auto& worker : workers)
            results.push_back(PairwiseWorkerCommandResult{
                .worker_id = worker.worker_id,
                .failure = std::nullopt,
                .operation_primary = std::nullopt,
                .operation_cleanup = std::nullopt,
                .cpu_before = std::nullopt,
                .cpu_after = std::nullopt,
                .migrated = false,
                .cancellation_observed = false,
                .peer_stopped = false});
    }

    struct SignalContext final {
        void* team{};
        State* generation{};
        std::size_t worker{};
    };

    PairwiseWorkerGenerationClass generation_class;
    std::stop_source stop;
    PairwiseWorkerOperation operation{};
    WorkerObservationPolicy observation{WorkerObservationPolicy::none};
    WorkerCancellationPolicy cancellation_policy{WorkerCancellationPolicy::observe};
    std::vector<PairwiseWorkerCommandResult> results;
    std::vector<std::uint8_t> timed;
    std::vector<std::optional<PairwiseWorkerFailure>> pre_timed_failures;
    std::vector<SignalContext> signal_contexts;
    std::uint64_t generation{};
    std::size_t armed{};
    std::size_t completed{};
    std::size_t timed_completed{};
    bool released{};
    bool cancelled{};
    bool cancelled_by_runner{};
    bool timed_required{true};
    std::optional<PairwiseWorkerFailure> terminal_failure;
    std::optional<PairwiseWorkerFailure> ordinary_failure;
    Phase phase{Phase::prepared};
};

struct PairwisePersistentWorkerTeam::State {
    enum class Phase : std::uint8_t {
        prepared, starting, ready, active, stopping, joined, fatal, count
    };

    State(std::vector<WorkerLaunchSpec> launch_specs,
          RunnerCancellation& cancellation_value,
          PairwisePlatformLeafFacade platform_value,
          std::vector<PairwisePreparedAffinityWorkspace> affinity_value,
          std::vector<core::CpuSet> singleton_value,
          std::vector<std::unique_ptr<PairwiseWorkerCommandOutcome::State>> generation_value,
          std::size_t force_capacity)
        : specs{std::move(launch_specs)}, cancellation{cancellation_value},
          platform{std::move(platform_value)}, affinity{std::move(affinity_value)},
          singletons{std::move(singleton_value)}, startup(specs.size()),
          generations{std::move(generation_value)}, force_errors(force_capacity) {
        threads.reserve(specs.size());
        readiness.reserve(specs.size());
        cleanup_failures.reserve(force_capacity);
    }

    std::vector<WorkerLaunchSpec> specs;
    RunnerCancellation& cancellation;
    PairwisePlatformLeafFacade platform;
    PairwiseWorkerOwningDependencies dependencies{PairwisePlatformLeafFacade::production(), {}};
    std::vector<PairwisePreparedAffinityWorkspace> affinity;
    std::vector<core::CpuSet> singletons;
    std::vector<std::optional<std::expected<WorkerReadyEvidence, PairwiseWorkerFailure>>> startup;
    std::vector<WorkerReadyEvidence> readiness;
    std::vector<std::jthread> threads;
    std::vector<std::unique_ptr<PairwiseWorkerCommandOutcome::State>> generations;
    std::vector<PairwiseWorkerFailure> cleanup_failures;
    std::vector<ForcedTeamStopError> force_errors;
    std::shared_ptr<PairwiseWorkerReadinessBorrowState> readiness_borrow{
        std::make_shared<PairwiseWorkerReadinessBorrowState>()};
    std::size_t force_error_count{};
    std::size_t memory_preparation_next{};
    std::size_t calibration_next{2U};
    std::size_t warmup_next{130U};
    std::size_t observation_next{};
    std::size_t cleanup_next{};
    std::size_t warmup_begin{130U};
    std::size_t observation_begin{};
    std::size_t cleanup_begin{};
    std::mutex mutex;
    std::condition_variable condition;
    std::stop_source team_stop;
    PairwiseWorkerCommandOutcome::State* active_generation{};
    std::uint64_t generation{};
    Phase state{Phase::prepared};
};

struct PairwisePreparedWorkerTeam::State {
    explicit State(std::unique_ptr<PairwisePersistentWorkerTeam::State> value) noexcept
        : team{std::move(value)} {}
    std::unique_ptr<PairwisePersistentWorkerTeam::State> team;
};

PairwiseWorkerCommandOutcome::PairwiseWorkerCommandOutcome(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PairwiseWorkerCommandOutcome::PairwiseWorkerCommandOutcome(
    PairwiseWorkerCommandOutcome&&) noexcept = default;
auto PairwiseWorkerCommandOutcome::operator=(PairwiseWorkerCommandOutcome&&) noexcept
    -> PairwiseWorkerCommandOutcome& = default;
PairwiseWorkerCommandOutcome::~PairwiseWorkerCommandOutcome() noexcept = default;

PairwisePreparedWorkerTeam::PairwisePreparedWorkerTeam(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}
PairwisePreparedWorkerTeam::PairwisePreparedWorkerTeam(
    PairwisePreparedWorkerTeam&&) noexcept = default;
auto PairwisePreparedWorkerTeam::operator=(PairwisePreparedWorkerTeam&&) noexcept
    -> PairwisePreparedWorkerTeam& = default;
PairwisePreparedWorkerTeam::~PairwisePreparedWorkerTeam() noexcept = default;

PairwisePersistentWorkerTeam::PairwisePersistentWorkerTeam(
    std::unique_ptr<State> state) noexcept : state_{std::move(state)} {}

namespace {

[[nodiscard]] auto platform_startup_failure(PairwiseFailureReason reason,
                                            PairwisePlatformFailure failure) noexcept
    -> PairwiseWorkerFailure {
    return placement_failure(reason, nested_context(failure));
}

[[nodiscard]] auto startup_one(PairwisePersistentWorkerTeam::State& team,
                               std::size_t index) noexcept
    -> std::expected<WorkerReadyEvidence, PairwiseWorkerFailure> {
    try {
        before(team.dependencies.hooks, PairwiseWorkerOwningOperation::startup, index);
        const auto& spec = team.specs[index];
        auto tid = team.platform.current_thread_id();
        if (!tid)
            return std::unexpected{platform_startup_failure(
                PairwiseFailureReason::worker_identity_failed, std::move(tid.error()))};
        auto applied = team.platform.set_current_thread_affinity_prepared(
            spec.requested_cpu, team.affinity[index]);
        if (!applied)
            return std::unexpected{platform_startup_failure(
                PairwiseFailureReason::affinity_apply_failed,
                std::move(applied.error()))};
        auto affinity = team.platform.current_thread_affinity_is_singleton_prepared(
            spec.requested_cpu, team.affinity[index]);
        if (!affinity)
            return std::unexpected{platform_startup_failure(
                PairwiseFailureReason::affinity_readback_failed,
                std::move(affinity.error()))};
        if (!*affinity)
            return std::unexpected{placement_failure(
                PairwiseFailureReason::affinity_mismatch,
                phase(PairwiseLifecyclePhase::worker_startup))};
        auto cpu = team.platform.current_cpu();
        if (!cpu)
            return std::unexpected{platform_startup_failure(
                PairwiseFailureReason::current_cpu_failed, std::move(cpu.error()))};
        if (*cpu != spec.requested_cpu)
            return std::unexpected{placement_failure(
                PairwiseFailureReason::current_cpu_mismatch,
                phase(PairwiseLifecyclePhase::worker_startup))};
        return WorkerReadyEvidence{.worker_index = spec.worker_index,
                                   .worker_id = spec.worker_id,
                                   .stable_pu = spec.stable_pu,
                                   .stable_core = spec.stable_core,
                                   .requested_cpu = spec.requested_cpu,
                                   .tid = *tid,
                                   .observed_affinity = std::move(team.singletons[index]),
                                   .observed_cpu = *cpu};
    } catch (const std::exception&) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_startup,
                                         PairwiseLifecyclePhase::worker_startup)};
    } catch (...) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_startup,
                                         PairwiseLifecyclePhase::worker_startup, true)};
    }
}

void signal_timed(void* opaque) noexcept {
    auto& context = *static_cast<PairwiseWorkerCommandOutcome::State::SignalContext*>(opaque);
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(context.team);
    std::lock_guard lock{team.mutex};
    auto& generation = *context.generation;
    if (team.active_generation != &generation || !generation.released ||
        generation.phase != PairwiseWorkerCommandOutcome::State::Phase::released) {
        if (!generation.terminal_failure)
            generation.terminal_failure = worker_failure(
                PairwiseFailureCode::worker_invariant_failed,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::timed_signal_before_release,
                phase(PairwiseLifecyclePhase::worker_execute));
        team.state = PairwisePersistentWorkerTeam::State::Phase::fatal;
        team.team_stop.request_stop();
        generation.stop.request_stop();
        team.condition.notify_all();
        return;
    }
    if (generation.timed[context.worker]) {
        if (!generation.terminal_failure)
            generation.terminal_failure = worker_failure(
                PairwiseFailureCode::worker_invariant_failed,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::duplicate_timed_completion_signal,
                phase(PairwiseLifecyclePhase::worker_execute));
        team.state = PairwisePersistentWorkerTeam::State::Phase::fatal;
        team.team_stop.request_stop();
        generation.stop.request_stop();
        team.condition.notify_all();
        return;
    }
    generation.timed[context.worker] = true;
    ++generation.timed_completed;
    team.condition.notify_all();
}

[[nodiscard]] auto run_command(PairwisePersistentWorkerTeam::State& team,
                               PairwiseWorkerCommandOutcome::State& generation,
                               std::size_t index) noexcept
    -> PairwiseWorkerCommandResult {
    const auto& spec = team.specs[index];
    PairwiseWorkerCommandResult result{.worker_id = spec.worker_id,
                                       .failure = std::nullopt,
                                       .operation_primary = std::nullopt,
                                       .operation_cleanup = std::nullopt,
                                       .cpu_before = std::nullopt,
                                       .cpu_after = std::nullopt,
                                       .migrated = false,
                                       .cancellation_observed = false,
                                       .peer_stopped = false};
    const auto fail = [&result](PairwiseWorkerFailure value) {
        result.failure = std::move(value);
    };
    try {
        before(team.dependencies.hooks, PairwiseWorkerOwningOperation::execute, index);
        if (team.team_stop.stop_requested()) {
            fail(command_failure(PairwiseFailureReason::team_stopped,
                                 phase(PairwiseLifecyclePhase::worker_command)));
            return result;
        }
        if (generation.stop.stop_requested()) {
            result.peer_stopped = true;
            fail(command_failure(PairwiseFailureReason::team_stopped,
                                 phase(PairwiseLifecyclePhase::worker_command)));
            return result;
        }
        if (generation.cancellation_policy == WorkerCancellationPolicy::observe &&
            team.cancellation.requested()) {
            result.cancellation_observed = true;
            fail(command_failure(PairwiseFailureReason::runner_cancelled,
                                 phase(PairwiseLifecyclePhase::worker_command)));
            return result;
        }
        if (generation.observation == WorkerObservationPolicy::before_and_after) {
            auto cpu = team.platform.current_cpu();
            if (!cpu) {
                fail(command_failure(PairwiseFailureReason::current_cpu_failed,
                                     nested_context(cpu.error())));
                return result;
            }
            result.cpu_before = *cpu;
            if (*cpu != spec.requested_cpu) {
                result.migrated = true;
                fail(command_failure(PairwiseFailureReason::worker_migrated,
                                     phase(PairwiseLifecyclePhase::worker_command)));
                return result;
            }
        }
        before(team.dependencies.hooks, PairwiseWorkerOwningOperation::callback, index);
        const WorkerExecutionContext context{
            .worker_index = spec.worker_index,
            .worker_id = spec.worker_id,
            .processing_unit = spec.processing_unit,
            .core = spec.core,
            .requested_cpu = spec.requested_cpu,
            .readiness = team.readiness[index],
        };
        const PairwiseWorkerExecutionControl control{
            .runner_cancellation = team.cancellation,
            .team_stop = team.team_stop.get_token(),
            .command_stop = generation.stop.get_token(),
            .signal_context = &generation.signal_contexts[index],
            .signal = signal_timed,
        };
        auto callback = generation.operation.run(generation.operation.context, context, control);
        if (!callback) {
            result.operation_primary = callback.error().primary.failure();
            if (callback.error().cleanup)
                result.operation_cleanup = callback.error().cleanup->failure();
            fail(command_failure(PairwiseFailureReason::worker_operation_failed,
                                 worker_operation_context(
                                     callback.error().primary.failure())));
        }
        if (generation.observation == WorkerObservationPolicy::before_and_after) {
            auto cpu = team.platform.current_cpu();
            if (!cpu) {
                if (!result.failure)
                    fail(command_failure(PairwiseFailureReason::current_cpu_failed,
                                         nested_context(cpu.error())));
            } else {
                result.cpu_after = *cpu;
                if (*cpu != spec.requested_cpu) {
                    result.migrated = true;
                    if (!result.failure)
                        fail(command_failure(PairwiseFailureReason::worker_migrated,
                                             phase(PairwiseLifecyclePhase::worker_command)));
                }
            }
        }
        result.cancellation_observed = result.cancellation_observed ||
            (generation.cancellation_policy == WorkerCancellationPolicy::observe &&
             team.cancellation.requested());
        return result;
    } catch (const std::exception&) {
        fail(contained(PairwiseFailureOperation::worker_callback,
                       PairwiseLifecyclePhase::worker_callback));
        return result;
    } catch (...) {
        fail(contained(PairwiseFailureOperation::worker_callback,
                       PairwiseLifecyclePhase::worker_callback, true));
        return result;
    }
}

void worker_entry(void* opaque, std::size_t index) noexcept {
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(opaque);
    auto startup = startup_one(team, index);
    {
        std::lock_guard lock{team.mutex};
        team.startup[index] = std::move(startup);
        team.condition.notify_all();
    }
    std::uint64_t seen{};
    for (;;) {
        PairwiseWorkerCommandOutcome::State* generation{};
        {
            std::unique_lock lock{team.mutex};
            team.condition.wait(lock, [&] {
                return team.team_stop.stop_requested() ||
                       (team.active_generation != nullptr &&
                        team.active_generation->generation != seen);
            });
            if (team.team_stop.stop_requested() &&
                (team.active_generation == nullptr ||
                 team.active_generation->generation == seen))
                return;
            generation = team.active_generation;
            seen = generation->generation;
            ++generation->armed;
            if (generation->armed == team.specs.size()) {
                generation->phase = PairwiseWorkerCommandOutcome::State::Phase::ready;
                team.condition.notify_all();
            }
            team.condition.wait(lock, [&] {
                return team.team_stop.stop_requested() || generation->released ||
                       generation->cancelled;
            });
            if (team.team_stop.stop_requested())
                generation->cancelled = true;
            if (generation->cancelled) {
                const auto runner_cancelled = generation->cancelled_by_runner;
                generation->results[index] = PairwiseWorkerCommandResult{
                    .worker_id = team.specs[index].worker_id,
                    .failure = command_failure(
                                               runner_cancelled
                                                   ? PairwiseFailureReason::runner_cancelled
                                                   : PairwiseFailureReason::team_stopped,
                                               phase(PairwiseLifecyclePhase::worker_command)),
                    .operation_primary = std::nullopt,
                    .operation_cleanup = std::nullopt,
                    .cpu_before = std::nullopt,
                    .cpu_after = std::nullopt,
                    .migrated = false,
                    .cancellation_observed = runner_cancelled,
                    .peer_stopped = false};
            }
        }
        if (!generation->cancelled) {
            auto result = run_command(team, *generation, index);
            try {
                before(team.dependencies.hooks,
                       PairwiseWorkerOwningOperation::result_publication, index);
            } catch (const std::exception&) {
                result.failure = contained(
                    PairwiseFailureOperation::worker_result_publication,
                    PairwiseLifecyclePhase::worker_projection);
            } catch (...) {
                result.failure = contained(
                    PairwiseFailureOperation::worker_result_publication,
                    PairwiseLifecyclePhase::worker_projection, true);
            }
            generation->results[index] = std::move(result);
        }
        {
            std::lock_guard lock{team.mutex};
            auto& result = generation->results[index];
            // Classify this worker's actual control while publishing its result,
            // never from tokens observed later while selecting another worker.
            // No cancellation branch supplies a false timed-completion signal.
            if (generation->timed_required && !generation->timed[index] && !result.failure) {
                const auto cancelled = generation->cancellation_policy == WorkerCancellationPolicy::observe &&
                                       team.cancellation.requested();
                const auto team_stopped = team.team_stop.stop_requested();
                const auto peer_stopped = generation->stop.stop_requested() && !team_stopped;
                if (cancelled || peer_stopped || team_stopped) {
                    result.failure = command_failure(
                        cancelled ? PairwiseFailureReason::runner_cancelled : PairwiseFailureReason::team_stopped,
                        phase(PairwiseLifecyclePhase::worker_command));
                    result.cancellation_observed = result.cancellation_observed || cancelled;
                    result.peer_stopped = result.peer_stopped || peer_stopped;
                }
            }
            if (result.failure && !generation->timed[index] && !result.peer_stopped) {
                generation->pre_timed_failures[index] = result.failure;
                generation->stop.request_stop();
            }
            ++generation->completed;
            if (generation->completed == team.specs.size()) {
                if (generation->cancelled && generation->cancelled_by_runner &&
                    !generation->terminal_failure)
                    generation->terminal_failure = worker_failure(
                        PairwiseFailureCode::worker_generation_cancelled,
                        PairwiseFailureOperation::worker_generation,
                        PairwiseFailureReason::runner_cancelled,
                        phase(PairwiseLifecyclePhase::worker_generation));
                if (!generation->cancelled) {
                    for (const auto& failure : generation->pre_timed_failures) {
                        if (failure) {
                            generation->ordinary_failure = failure;
                            break;
                        }
                    }
                    if (!generation->ordinary_failure) {
                        for (const auto& value : generation->results) {
                            if (value.failure && !value.peer_stopped) {
                                generation->ordinary_failure = value.failure;
                                break;
                            }
                        }
                    }
                    const auto successful_without_signal = [&] {
                        if (!generation->timed_required)
                            return false;
                        for (std::size_t worker = 0; worker < generation->results.size(); ++worker)
                            if (!generation->results[worker].failure &&
                                !generation->timed[worker])
                                return true;
                        return false;
                    }();
                    if (successful_without_signal && !generation->terminal_failure)
                        generation->terminal_failure = worker_failure(
                            PairwiseFailureCode::worker_invariant_failed,
                            PairwiseFailureOperation::worker_generation,
                            PairwiseFailureReason::missing_timed_completion_signal,
                            phase(PairwiseLifecyclePhase::worker_execute));
                }
                generation->phase = PairwiseWorkerCommandOutcome::State::Phase::terminal;
                team.active_generation = nullptr;
                if (team.state != PairwisePersistentWorkerTeam::State::Phase::fatal)
                    team.state = PairwisePersistentWorkerTeam::State::Phase::ready;
                team.condition.notify_all();
            }
        }
    }
}

[[nodiscard]] auto all_startup_reported(const PairwisePersistentWorkerTeam::State& team) noexcept
    -> bool {
    return std::ranges::all_of(team.startup, [](const auto& value) {
        return value.has_value();
    });
}

[[nodiscard]] auto generation_slot(PairwisePersistentWorkerTeam::State& team,
                                   PairwiseWorkerGenerationClass value) noexcept
    -> std::unique_ptr<PairwiseWorkerCommandOutcome::State> {
    std::size_t* next{};
    std::size_t end{};
    switch (value) {
    case PairwiseWorkerGenerationClass::memory_preparation:
        next = &team.memory_preparation_next;
        end = 2U;
        break;
    case PairwiseWorkerGenerationClass::calibration:
        next = &team.calibration_next;
        end = 130U;
        break;
    case PairwiseWorkerGenerationClass::warmup:
        next = &team.warmup_next;
        end = team.observation_begin;
        break;
    case PairwiseWorkerGenerationClass::observation:
        next = &team.observation_next;
        end = team.cleanup_begin;
        break;
    case PairwiseWorkerGenerationClass::finalization_cleanup:
        next = &team.cleanup_next;
        end = team.generations.size();
        break;
    }
    if (next == nullptr || *next >= end)
        return {};
    auto result = std::move(team.generations[*next]);
    ++*next;
    return result;
}

void shutdown_threads(PairwisePersistentWorkerTeam::State& team,
                      std::optional<PairwiseWorkerFailure>* primary) noexcept {
    team.force_error_count = 0U;
    auto ordinary_recorded = false;
    const auto record = [&](PairwiseWorkerFailure failure,
                            ForcedTeamStopErrorKind kind, std::uint8_t sweep,
                            std::size_t worker, bool forced) noexcept {
        if (forced) {
            if (team.force_error_count >= team.force_errors.size())
                std::terminate();
            team.force_errors[team.force_error_count++] = ForcedTeamStopError{
                .kind = kind,
                .sweep = sweep,
                .worker_index = static_cast<std::uint32_t>(worker)};
            team.cleanup_failures.push_back(std::move(failure));
            return;
        }
        if (ordinary_recorded)
            return;
        ordinary_recorded = true;
        if (primary != nullptr && !*primary)
            *primary = std::move(failure);
        else
            team.cleanup_failures.push_back(std::move(failure));
    };
    const auto attempt = [&](std::uint8_t sweep, bool forced) noexcept {
        for (std::size_t index = 0; index < team.threads.size(); ++index) {
            auto& thread = team.threads[index];
            if (!thread.joinable())
                continue;
            thread.request_stop();
            auto injected_failure = false;
            auto end_sweep = false;
            try {
                if (team.dependencies.hooks.before_join != nullptr)
                    team.dependencies.hooks.before_join(
                        team.dependencies.hooks.context, index);
            } catch (const std::system_error& error) {
                record(worker_failure(
                           PairwiseFailureCode::worker_stop_failed,
                           PairwiseFailureOperation::worker_join,
                           PairwiseFailureReason::join_failed,
                           phase(PairwiseLifecyclePhase::worker_join),
                           !forced && error.code().value() > 0 ?
                               std::optional<int>{error.code().value()} : std::nullopt),
                       ForcedTeamStopErrorKind::join_failed, sweep, index, forced);
                injected_failure = true;
            } catch (const std::exception&) {
                record(contained(PairwiseFailureOperation::worker_join,
                                 PairwiseLifecyclePhase::worker_join),
                       ForcedTeamStopErrorKind::contained_exception, sweep, index, forced);
                injected_failure = true;
                end_sweep = forced;
            } catch (...) {
                record(contained(PairwiseFailureOperation::worker_join,
                                 PairwiseLifecyclePhase::worker_join, true),
                       ForcedTeamStopErrorKind::contained_unknown_failure, sweep, index, forced);
                injected_failure = true;
                end_sweep = forced;
            }
            if (end_sweep)
                break;
            if (injected_failure)
                continue;
            try {
                thread.join();
            } catch (const std::system_error& error) {
                record(worker_failure(
                           PairwiseFailureCode::worker_stop_failed,
                           PairwiseFailureOperation::worker_join,
                           PairwiseFailureReason::join_failed,
                           phase(PairwiseLifecyclePhase::worker_join),
                           !forced && error.code().value() > 0 ?
                               std::optional<int>{error.code().value()} : std::nullopt),
                       ForcedTeamStopErrorKind::join_failed, sweep, index, forced);
            } catch (const std::exception&) {
                record(contained(PairwiseFailureOperation::worker_join,
                                 PairwiseLifecyclePhase::worker_join),
                       ForcedTeamStopErrorKind::contained_exception, sweep, index, forced);
                if (forced)
                    break;
            } catch (...) {
                record(contained(PairwiseFailureOperation::worker_join,
                                 PairwiseLifecyclePhase::worker_join, true),
                       ForcedTeamStopErrorKind::contained_unknown_failure, sweep, index, forced);
                if (forced)
                    break;
            }
        }
    };

    // The ordinary stop attempt owns the primary error but is deliberately not
    // part of the fixed forced-stop POD ledger.  Only retained joinable owners
    // enter the two complete, one-based forced sweeps.
    attempt(0U, false);
    for (std::uint8_t sweep = 1U; sweep <= 2U; ++sweep) {
        if (std::ranges::none_of(team.threads,
                                [](const auto& thread) { return thread.joinable(); }))
            break;
        attempt(sweep, true);
    }
    if (std::ranges::any_of(team.threads,
                            [](const auto& thread) { return thread.joinable(); }))
        std::terminate();
}

} // namespace

void PairwiseWorkerOwningTestAccess::signal(
    PairwisePersistentWorkerGeneration& generation, std::size_t worker) noexcept {
    if (!generation.state_ || worker >= generation.state_->signal_contexts.size())
        std::terminate();
    signal_timed(&generation.state_->signal_contexts[worker]);
}

auto PairwiseWorkerOwningTestAccess::author_failure(
    PairwiseFailureCode code, PairwiseFailureOperation operation,
    PairwiseFailureReason reason, PairwiseFailureContext context,
    std::optional<int> error_number) noexcept -> PairwiseWorkerFailure {
    return worker_failure(code, operation, reason, std::move(context), error_number);
}

auto PairwiseWorkerOwningTestAccess::forced_errors(
    const PairwisePersistentWorkerTeam& team) noexcept
    -> std::span<const ForcedTeamStopError> {
    if (!team.state_)
        return {};
    return {team.state_->force_errors.data(), team.state_->force_error_count};
}

auto PairwiseWorkerCommandOutcome::workers() const noexcept
    -> std::span<const PairwiseWorkerCommandResult> {
    return state_ ? std::span<const PairwiseWorkerCommandResult>{state_->results} :
                    std::span<const PairwiseWorkerCommandResult>{};
}

auto PairwiseWorkerCommandOutcome::cancelled() const noexcept -> bool {
    return state_ && state_->cancelled;
}

auto PairwisePreparedWorkerTeam::worker_count() const noexcept -> std::size_t {
    return state_ && state_->team ? state_->team->specs.size() : 0U;
}

auto PairwisePreparedWorkerTeam::generation_capacity() const noexcept -> std::size_t {
    return state_ && state_->team ? state_->team->generations.size() : 0U;
}

PairwisePersistentWorkerTeam::PairwisePersistentWorkerTeam(
    PairwisePersistentWorkerTeam&&) noexcept = default;

auto PairwisePersistentWorkerTeam::operator=(PairwisePersistentWorkerTeam&& other) noexcept
    -> PairwisePersistentWorkerTeam& {
    if (this != &other) {
        if (active())
            std::terminate();
        if (state_ && state_->readiness_borrow)
            state_->readiness_borrow->team = nullptr;
        state_ = std::move(other.state_);
    }
    return *this;
}

PairwisePersistentWorkerTeam::~PairwisePersistentWorkerTeam() noexcept {
    if (active())
        std::terminate();
    if (state_ && state_->readiness_borrow)
        state_->readiness_borrow->team = nullptr;
}

auto PairwisePersistentWorkerTeam::readiness() const noexcept
    -> std::span<const WorkerReadyEvidence> {
    return state_ ? std::span<const WorkerReadyEvidence>{state_->readiness} :
                    std::span<const WorkerReadyEvidence>{};
}

auto PairwisePersistentWorkerTeam::lend_monitoring_readiness() & noexcept
    -> std::optional<PairwiseWorkerReadinessCapability> {
    if (!state_)
        return std::nullopt;
    std::lock_guard lock{state_->mutex};
    auto& borrow = *state_->readiness_borrow;
    if (state_->state != State::Phase::ready || borrow.outstanding ||
        state_->readiness.size() != state_->specs.size() ||
        borrow.generation == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    ++borrow.generation;
    borrow.outstanding = true;
    return PairwiseWorkerReadinessCapability{state_->readiness_borrow,
                                             borrow.generation};
}

PairwiseWorkerReadinessCapability::PairwiseWorkerReadinessCapability(
    PairwiseWorkerReadinessCapability&& other) noexcept
    : borrow_{std::move(other.borrow_)}, generation_{other.generation_},
      consumed_{other.consumed_} {
    other.consumed_ = true;
}

auto PairwiseWorkerReadinessCapability::operator=(
    PairwiseWorkerReadinessCapability&& other) noexcept
    -> PairwiseWorkerReadinessCapability& {
    if (this != &other) {
        release();
        borrow_ = std::move(other.borrow_);
        generation_ = other.generation_;
        consumed_ = other.consumed_;
        other.consumed_ = true;
    }
    return *this;
}

PairwiseWorkerReadinessCapability::~PairwiseWorkerReadinessCapability() noexcept {
    release();
}

void PairwiseWorkerReadinessCapability::release() noexcept {
    if (consumed_)
        return;
    consumed_ = true;
    const auto borrow = borrow_.lock();
    if (borrow && borrow->outstanding && borrow->generation == generation_)
        borrow->outstanding = false;
}

auto PairwiseWorkerReadinessCapability::consume() noexcept
    -> std::pair<std::span<const WorkerLaunchSpec>,
                 std::span<const WorkerReadyEvidence>> {
    if (consumed_)
        return {};
    consumed_ = true;
    const auto borrow = borrow_.lock();
    if (!borrow || !borrow->team || !borrow->outstanding ||
        borrow->generation != generation_)
        return {};
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(borrow->team);
    std::lock_guard lock{team.mutex};
    if (team.state != PairwisePersistentWorkerTeam::State::Phase::ready ||
        team.readiness.size() != team.specs.size()) {
        borrow->outstanding = false;
        return {};
    }
    borrow->outstanding = false;
    return {{team.specs.data(), team.specs.size()},
            {team.readiness.data(), team.readiness.size()}};
}

auto PairwisePersistentWorkerTeam::ready() const noexcept -> bool {
    if (!state_)
        return false;
    std::lock_guard lock{state_->mutex};
    return state_->state == State::Phase::ready;
}

auto PairwisePersistentWorkerTeam::progress_quiescent() const noexcept -> bool {
    if (!state_) return false;
    std::lock_guard lock{state_->mutex};
    return state_->active_generation == nullptr &&
        (state_->state == State::Phase::ready || state_->state == State::Phase::joined ||
         state_->state == State::Phase::fatal);
}

auto PairwisePersistentWorkerTeam::active() const noexcept -> bool {
    if (!state_)
        return false;
    return std::ranges::any_of(state_->threads,
                               [](const auto& thread) { return thread.joinable(); });
}

auto PairwisePersistentWorkerTeam::begin_generation(
    PairwiseWorkerGenerationClass generation_class,
    PairwiseWorkerOperation operation,
    WorkerObservationPolicy observation,
    WorkerCancellationPolicy cancellation_policy) noexcept
    -> std::expected<PairwisePersistentWorkerGeneration, PairwiseWorkerFailure> {
    if (!state_)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::team_not_ready,
            phase(PairwiseLifecyclePhase::worker_generation))};
    try {
        before(state_->dependencies.hooks, PairwiseWorkerOwningOperation::generation, 0U);
        std::lock_guard lock{state_->mutex};
        auto observed_state = static_cast<std::uint8_t>(state_->state);
        if (state_->dependencies.hooks.adjust_team_state != nullptr)
            state_->dependencies.hooks.adjust_team_state(
                state_->dependencies.hooks.context, observed_state);
        if (observed_state >= static_cast<std::uint8_t>(State::Phase::count))
            return std::unexpected{worker_failure(
                PairwiseFailureCode::worker_invariant_failed,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::unexpected_worker_state,
                phase(PairwiseLifecyclePhase::worker_generation))};
        if (operation.run == nullptr)
            return std::unexpected{worker_failure(
                PairwiseFailureCode::worker_invalid_argument,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::missing_operation,
                phase(PairwiseLifecyclePhase::worker_generation))};
        if (state_->state == State::Phase::active)
            return std::unexpected{worker_failure(
                PairwiseFailureCode::worker_invalid_state,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::command_active,
                phase(PairwiseLifecyclePhase::worker_generation))};
        if (state_->state != State::Phase::ready)
            return std::unexpected{worker_failure(
                PairwiseFailureCode::worker_invalid_state,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::team_not_ready,
                phase(PairwiseLifecyclePhase::worker_generation))};
        auto observed_generation = state_->generation;
        if (state_->dependencies.hooks.adjust_generation != nullptr)
            state_->dependencies.hooks.adjust_generation(
                state_->dependencies.hooks.context, observed_generation);
        if (observed_generation == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected{worker_failure(
                PairwiseFailureCode::worker_invariant_failed,
                PairwiseFailureOperation::worker_generation,
                PairwiseFailureReason::generation_exhausted,
                phase(PairwiseLifecyclePhase::worker_generation))};
        auto generation = generation_slot(*state_, generation_class);
        if (!generation)
            std::terminate();
        ++state_->generation;
        generation->generation = state_->generation;
        generation->operation = operation;
        generation->observation = observation;
        generation->cancellation_policy = cancellation_policy;
        generation->phase = PairwiseWorkerCommandOutcome::State::Phase::arming;
        for (std::size_t index = 0; index < generation->signal_contexts.size(); ++index)
            generation->signal_contexts[index] = {.team = state_.get(),
                                                  .generation = generation.get(),
                                                  .worker = index};
        state_->active_generation = generation.get();
        state_->state = State::Phase::active;
        state_->condition.notify_all();
        return PairwisePersistentWorkerGeneration{state_.get(), std::move(generation)};
    } catch (const std::exception&) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_generation,
                                         PairwiseLifecyclePhase::worker_generation)};
    } catch (...) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_generation,
                                         PairwiseLifecyclePhase::worker_generation, true)};
    }
}

auto PairwisePersistentWorkerTeam::execute(
    PairwiseWorkerGenerationClass generation_class,
    PairwiseWorkerOperation operation,
    WorkerObservationPolicy observation,
    WorkerCancellationPolicy cancellation_policy) noexcept
    -> std::expected<PairwiseWorkerCommandOutcome, PairwiseWorkerFailure> {
    try {
        if (!state_)
            return std::unexpected{worker_failure(
                PairwiseFailureCode::worker_invalid_state,
                PairwiseFailureOperation::worker_execute,
                PairwiseFailureReason::team_not_ready,
                phase(PairwiseLifecyclePhase::worker_execute))};
        before(state_->dependencies.hooks, PairwiseWorkerOwningOperation::execute, 0U);
        {
            std::lock_guard lock{state_->mutex};
            auto observed_state = static_cast<std::uint8_t>(state_->state);
            if (state_->dependencies.hooks.adjust_team_state != nullptr)
                state_->dependencies.hooks.adjust_team_state(
                    state_->dependencies.hooks.context, observed_state);
            if (observed_state >= static_cast<std::uint8_t>(State::Phase::count))
                return std::unexpected{worker_failure(
                    PairwiseFailureCode::worker_invariant_failed,
                    PairwiseFailureOperation::worker_execute,
                    PairwiseFailureReason::unexpected_worker_state,
                    phase(PairwiseLifecyclePhase::worker_execute))};
            auto observed_generation = state_->generation;
            if (state_->dependencies.hooks.adjust_generation != nullptr)
                state_->dependencies.hooks.adjust_generation(
                    state_->dependencies.hooks.context, observed_generation);
            if (observed_generation == std::numeric_limits<std::uint64_t>::max())
                return std::unexpected{worker_failure(
                    PairwiseFailureCode::worker_invariant_failed,
                    PairwiseFailureOperation::worker_execute,
                    PairwiseFailureReason::generation_exhausted,
                    phase(PairwiseLifecyclePhase::worker_execute))};
            if (operation.run == nullptr)
                return std::unexpected{worker_failure(
                    PairwiseFailureCode::worker_invalid_argument,
                    PairwiseFailureOperation::worker_execute,
                    PairwiseFailureReason::missing_operation,
                    phase(PairwiseLifecyclePhase::worker_execute))};
            if (state_->state == State::Phase::active)
                return std::unexpected{worker_failure(
                    PairwiseFailureCode::worker_invalid_state,
                    PairwiseFailureOperation::worker_execute,
                    PairwiseFailureReason::command_active,
                    phase(PairwiseLifecyclePhase::worker_execute))};
            if (state_->state == State::Phase::stopping ||
                state_->state == State::Phase::joined)
                return std::unexpected{worker_failure(
                    PairwiseFailureCode::worker_invalid_state,
                    PairwiseFailureOperation::worker_execute,
                    PairwiseFailureReason::command_after_stop,
                    phase(PairwiseLifecyclePhase::worker_execute))};
            if (state_->state != State::Phase::ready)
                return std::unexpected{worker_failure(
                    PairwiseFailureCode::worker_invalid_state,
                    PairwiseFailureOperation::worker_execute,
                    PairwiseFailureReason::team_not_ready,
                    phase(PairwiseLifecyclePhase::worker_execute))};
        }
        auto generation = begin_generation(generation_class, operation, observation,
                                           cancellation_policy);
        if (!generation)
            return std::unexpected{std::move(generation.error())};
        generation->state_->timed_required = false;
        auto ready = generation->await_ready();
        if (!ready)
            return std::unexpected{std::move(ready.error())};
        generation->release();
        const auto quiescent = generation->await_quiescence();
        if (!quiescent && generation->state_->terminal_failure)
            return std::unexpected{std::move(quiescent.error())};
        return generation->collect_ordered_results();
    } catch (const std::exception&) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_execute,
                                         PairwiseLifecyclePhase::worker_execute)};
    } catch (...) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_execute,
                                         PairwiseLifecyclePhase::worker_execute, true)};
    }
}

auto PairwisePersistentWorkerTeam::stop_and_join() noexcept
    -> std::expected<void, PairwiseWorkerFailureSequence> {
    if (!state_)
        return {};
    std::optional<PairwiseWorkerFailure> primary;
    {
        std::lock_guard lock{state_->mutex};
        state_->state = State::Phase::stopping;
        state_->team_stop.request_stop();
        if (state_->active_generation)
            state_->active_generation->stop.request_stop();
        state_->condition.notify_all();
    }
    state_->cleanup_failures.clear();
    shutdown_threads(*state_, &primary);
    {
        std::lock_guard lock{state_->mutex};
        state_->state = State::Phase::joined;
    }
    if (primary)
        return std::unexpected{PairwiseWorkerFailureSequence{
            .primary = std::move(*primary),
            .cleanup = std::move(state_->cleanup_failures)}};
    return {};
}

PairwisePersistentWorkerGeneration::PairwisePersistentWorkerGeneration(
    PairwisePersistentWorkerGeneration&&) noexcept = default;

PairwisePersistentWorkerGeneration::PairwisePersistentWorkerGeneration(
    void* team, std::unique_ptr<PairwiseWorkerCommandOutcome::State> state) noexcept
    : team_{team}, state_{std::move(state)} {}

auto PairwisePersistentWorkerGeneration::operator=(
    PairwisePersistentWorkerGeneration&& other) noexcept
    -> PairwisePersistentWorkerGeneration& {
    if (this != &other) {
        if (state_ && state_->phase != PairwiseWorkerCommandOutcome::State::Phase::terminal &&
            state_->phase != PairwiseWorkerCommandOutcome::State::Phase::collected)
            std::terminate();
        team_ = std::exchange(other.team_, nullptr);
        state_ = std::move(other.state_);
        collected_ = std::exchange(other.collected_, false);
    }
    return *this;
}

PairwisePersistentWorkerGeneration::~PairwisePersistentWorkerGeneration() noexcept {
    if (state_ && state_->phase != PairwiseWorkerCommandOutcome::State::Phase::terminal &&
        state_->phase != PairwiseWorkerCommandOutcome::State::Phase::collected)
        std::terminate();
}

auto PairwisePersistentWorkerGeneration::await_ready() noexcept
    -> std::expected<void, PairwiseWorkerFailure> {
    if (!team_ || !state_)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_invalid_phase,
            phase(PairwiseLifecyclePhase::worker_generation))};
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{team.mutex};
    if (state_->phase != PairwiseWorkerCommandOutcome::State::Phase::arming &&
        state_->phase != PairwiseWorkerCommandOutcome::State::Phase::ready)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_invalid_phase,
            phase(PairwiseLifecyclePhase::worker_generation))};
    team.condition.wait(lock, [&] {
        return state_->phase == PairwiseWorkerCommandOutcome::State::Phase::ready ||
               state_->terminal_failure.has_value() || team.team_stop.stop_requested();
    });
    if (state_->terminal_failure)
        return std::unexpected{*state_->terminal_failure};
    if (state_->ordinary_failure)
        return std::unexpected{*state_->ordinary_failure};
    return {};
}

void PairwisePersistentWorkerGeneration::cancel_before_release() noexcept {
    if (!team_ || !state_)
        return;
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(team_);
    std::lock_guard lock{team.mutex};
    if (state_->phase != PairwiseWorkerCommandOutcome::State::Phase::ready)
        std::terminate();
    state_->cancelled = true;
    state_->cancelled_by_runner = true;
    state_->stop.request_stop();
    team.condition.notify_all();
}

void PairwisePersistentWorkerGeneration::release() noexcept {
    if (!team_ || !state_)
        return;
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(team_);
    std::lock_guard lock{team.mutex};
    if (state_->phase != PairwiseWorkerCommandOutcome::State::Phase::ready)
        std::terminate();
    state_->released = true;
    state_->phase = PairwiseWorkerCommandOutcome::State::Phase::released;
    team.condition.notify_all();
}

auto PairwisePersistentWorkerGeneration::await_timed_completion() noexcept
    -> std::expected<void, PairwiseWorkerFailure> {
    if (!team_ || !state_)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_invalid_phase,
            phase(PairwiseLifecyclePhase::worker_generation))};
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{team.mutex};
    if (state_->phase != PairwiseWorkerCommandOutcome::State::Phase::released &&
        state_->phase != PairwiseWorkerCommandOutcome::State::Phase::terminal)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_invalid_phase,
            phase(PairwiseLifecyclePhase::worker_generation))};
    team.condition.wait(lock, [&] {
        return state_->timed_completed == state_->results.size() ||
               state_->phase == PairwiseWorkerCommandOutcome::State::Phase::terminal ||
               state_->terminal_failure.has_value();
    });
    if (state_->terminal_failure)
        return std::unexpected{*state_->terminal_failure};
    if (state_->ordinary_failure)
        return std::unexpected{*state_->ordinary_failure};
    return {};
}

auto PairwisePersistentWorkerGeneration::await_quiescence() noexcept
    -> std::expected<void, PairwiseWorkerFailure> {
    if (!team_ || !state_)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_invalid_phase,
            phase(PairwiseLifecyclePhase::worker_generation))};
    auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(team_);
    std::unique_lock lock{team.mutex};
    team.condition.wait(lock, [&] {
        return state_->phase == PairwiseWorkerCommandOutcome::State::Phase::terminal;
    });
    if (state_->terminal_failure)
        return std::unexpected{*state_->terminal_failure};
    if (state_->ordinary_failure)
        return std::unexpected{*state_->ordinary_failure};
    return {};
}

auto PairwisePersistentWorkerGeneration::collect_ordered_results() noexcept
    -> std::expected<PairwiseWorkerCommandOutcome, PairwiseWorkerFailure> {
    if (collected_)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_result_already_collected,
            phase(PairwiseLifecyclePhase::worker_projection))};
    if (!state_ || state_->phase != PairwiseWorkerCommandOutcome::State::Phase::terminal)
        return std::unexpected{worker_failure(
            PairwiseFailureCode::worker_invalid_state,
            PairwiseFailureOperation::worker_generation,
            PairwiseFailureReason::generation_result_unavailable,
            phase(PairwiseLifecyclePhase::worker_projection))};
    if (state_->terminal_failure)
        return std::unexpected{*state_->terminal_failure};
    try {
        auto& team = *static_cast<PairwisePersistentWorkerTeam::State*>(team_);
        before(team.dependencies.hooks, PairwiseWorkerOwningOperation::projection, 0U);
    } catch (const std::exception&) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_projection,
                                         PairwiseLifecyclePhase::worker_projection)};
    } catch (...) {
        return std::unexpected{contained(PairwiseFailureOperation::worker_projection,
                                         PairwiseLifecyclePhase::worker_projection, true)};
    }
    state_->phase = PairwiseWorkerCommandOutcome::State::Phase::collected;
    collected_ = true;
    team_ = nullptr;
    return PairwiseWorkerCommandOutcome{std::move(state_)};
}

auto PairwiseWorkerOwningFacade::production_dependencies() noexcept
    -> PairwiseWorkerOwningDependencies {
    return {.platform = PairwisePlatformLeafFacade::production(),
            .launcher = {.context = nullptr, .launch = production_launch}};
}

auto PairwiseWorkerOwningFacade::prepare(
    std::span<const WorkerLaunchSpec> workers,
    RunnerCancellation& cancellation,
    const measurement::WarmupPolicy& warmup,
    const measurement::RepetitionPolicy& repetition,
    PairwisePlatformLeafFacade platform) noexcept
    -> std::expected<PairwisePreparedWorkerTeam, PairwiseEvidenceFailure> {
    if (workers.empty())
        return std::unexpected{PairwiseEvidenceFailure{worker_failure(
            PairwiseFailureCode::worker_invalid_argument,
            PairwiseFailureOperation::worker_team_create,
            PairwiseFailureReason::empty_worker_set,
            phase(PairwiseLifecyclePhase::create))}};
    for (std::size_t left = 0; left < workers.size(); ++left) {
        if (workers[left].worker_index != left)
            return std::unexpected{PairwiseEvidenceFailure{worker_failure(
                PairwiseFailureCode::worker_invalid_argument,
                PairwiseFailureOperation::worker_team_create,
                PairwiseFailureReason::worker_id_overflow,
                phase(PairwiseLifecyclePhase::create))}};
        for (std::size_t right = left + 1U; right < workers.size(); ++right) {
            if (workers[left].worker_id == workers[right].worker_id)
                return std::unexpected{PairwiseEvidenceFailure{worker_failure(
                    PairwiseFailureCode::worker_invariant_failed,
                    PairwiseFailureOperation::worker_team_create,
                    PairwiseFailureReason::duplicate_worker,
                    phase(PairwiseLifecyclePhase::create))}};
            if (workers[left].requested_cpu == workers[right].requested_cpu)
                return std::unexpected{PairwiseEvidenceFailure{worker_failure(
                    PairwiseFailureCode::worker_invariant_failed,
                    PairwiseFailureOperation::worker_team_create,
                    PairwiseFailureReason::duplicate_cpu,
                    phase(PairwiseLifecyclePhase::create))}};
        }
    }
    const auto warmup_maximum_trials = warmup.maximum_trials();
    const auto repetition_maximum_blocks = repetition.maximum_blocks();
    const auto capacity = checked_capacity(warmup_maximum_trials,
                                           repetition_maximum_blocks);
    if (!capacity)
        return std::unexpected{PairwiseEvidenceFailure{runtime_preparation_failure()}};
    try {
        std::vector<WorkerLaunchSpec> specs{workers.begin(), workers.end()};
        if (workers.size() > (std::numeric_limits<std::size_t>::max() - 3U) / 2U)
            return std::unexpected{PairwiseEvidenceFailure{runtime_preparation_failure()}};
        std::vector<PairwisePreparedAffinityWorkspace> affinity;
        std::vector<core::CpuSet> singletons;
        affinity.reserve(workers.size());
        singletons.reserve(workers.size());
        std::size_t minimum_capacity{};
        for (const auto& worker : workers)
            minimum_capacity = std::max(
                minimum_capacity,
                static_cast<std::size_t>(worker.requested_cpu.value()) + 1U);
        auto first_workspace = platform.prepare_affinity_workspace(minimum_capacity);
        if (!first_workspace)
            return std::unexpected{PairwiseEvidenceFailure{
                std::move(first_workspace.error())}};
        const auto affinity_capacity = first_workspace->capacity();
        affinity.push_back(std::move(*first_workspace));
        for (std::size_t index = 1U; index < workers.size(); ++index) {
            auto workspace = platform.prepare_affinity_workspace_exact(affinity_capacity);
            if (!workspace)
                return std::unexpected{PairwiseEvidenceFailure{
                    std::move(workspace.error())}};
            affinity.push_back(std::move(*workspace));
        }
        for (const auto& worker : workers) {
            singletons.emplace_back(
                std::initializer_list<core::LinuxCpuId>{worker.requested_cpu});
        }
        std::vector<std::unique_ptr<PairwiseWorkerCommandOutcome::State>> generations;
        generations.reserve(*capacity);
        const auto warmup_begin = std::size_t{130U};
        const auto observation_begin = warmup_begin +
            static_cast<std::size_t>(warmup_maximum_trials * 2U);
        const auto cleanup_begin = observation_begin +
            static_cast<std::size_t>(repetition_maximum_blocks * 4U);
        for (std::size_t index = 0; index < *capacity; ++index) {
            auto generation_class = PairwiseWorkerGenerationClass::finalization_cleanup;
            if (index < 2U)
                generation_class = PairwiseWorkerGenerationClass::memory_preparation;
            else if (index < 130U)
                generation_class = PairwiseWorkerGenerationClass::calibration;
            else if (index < observation_begin)
                generation_class = PairwiseWorkerGenerationClass::warmup;
            else if (index < cleanup_begin)
                generation_class = PairwiseWorkerGenerationClass::observation;
            generations.push_back(std::make_unique<PairwiseWorkerCommandOutcome::State>(
                generation_class, workers));
        }
        auto team = std::make_unique<PairwisePersistentWorkerTeam::State>(
            std::move(specs), cancellation, std::move(platform), std::move(affinity),
            std::move(singletons), std::move(generations), workers.size() * 2U + 3U);
        team->readiness_borrow->team = team.get();
        team->warmup_begin = warmup_begin;
        team->observation_begin = observation_begin;
        team->observation_next = observation_begin;
        team->cleanup_begin = cleanup_begin;
        team->cleanup_next = cleanup_begin;
        return PairwisePreparedWorkerTeam{
            std::make_unique<PairwisePreparedWorkerTeam::State>(std::move(team))};
    } catch (...) {
        return std::unexpected{PairwiseEvidenceFailure{runtime_preparation_failure()}};
    }
}

auto PairwiseWorkerOwningFacade::start(
    PairwisePreparedWorkerTeam prepared,
    PairwiseWorkerOwningDependencies dependencies) noexcept
    -> std::expected<PairwisePersistentWorkerTeam, PairwiseWorkerFailureSequence> {
    auto team = prepared.state_ ? std::move(prepared.state_->team) : nullptr;
    if (!team || dependencies.launcher.launch == nullptr) {
        auto primary = worker_failure(PairwiseFailureCode::worker_invalid_argument,
                                      PairwiseFailureOperation::worker_team_create,
                                      PairwiseFailureReason::empty_worker_set,
                                      phase(PairwiseLifecyclePhase::create));
        return std::unexpected{PairwiseWorkerFailureSequence{
            .primary = std::move(primary), .cleanup = {}}};
    }
    team->dependencies = std::move(dependencies);
    team->platform = team->dependencies.platform;
    team->state = PairwisePersistentWorkerTeam::State::Phase::starting;
    std::optional<PairwiseWorkerFailure> launch_failure;
    try {
        for (std::size_t index = 0; index < team->specs.size(); ++index) {
            before(team->dependencies.hooks, PairwiseWorkerOwningOperation::launch, index);
            try {
                team->threads.push_back(team->dependencies.launcher.launch(
                    team->dependencies.launcher.context, worker_entry, team.get(), index));
            } catch (const std::system_error&) {
                launch_failure = worker_failure(
                    PairwiseFailureCode::worker_start_failed,
                    PairwiseFailureOperation::worker_launch,
                    PairwiseFailureReason::thread_launch_failed,
                    phase(PairwiseLifecyclePhase::worker_launch));
                break;
            } catch (const std::exception&) {
                launch_failure = contained(PairwiseFailureOperation::worker_launch,
                                           PairwiseLifecyclePhase::worker_launch);
                break;
            } catch (...) {
                launch_failure = contained(PairwiseFailureOperation::worker_launch,
                                           PairwiseLifecyclePhase::worker_launch, true);
                break;
            }
        }
    } catch (const std::exception&) {
        launch_failure = contained(PairwiseFailureOperation::worker_team_create,
                                   PairwiseLifecyclePhase::create);
    } catch (...) {
        launch_failure = contained(PairwiseFailureOperation::worker_team_create,
                                   PairwiseLifecyclePhase::create, true);
    }
    if (!launch_failure) {
        std::unique_lock lock{team->mutex};
        team->condition.wait(lock, [&] { return all_startup_reported(*team); });
    }
    std::optional<PairwiseWorkerFailure> startup_failure;
    if (!launch_failure) {
        for (const auto& value : team->startup) {
            if (value && !*value) {
                startup_failure = value->error();
                break;
            }
        }
    }
    if (launch_failure || startup_failure) {
        {
            std::lock_guard lock{team->mutex};
            team->team_stop.request_stop();
            team->condition.notify_all();
        }
        auto primary = worker_failure(
            PairwiseFailureCode::worker_start_failed,
            PairwiseFailureOperation::worker_team_create,
            launch_failure ? PairwiseFailureReason::thread_launch_failed
                           : PairwiseFailureReason::worker_not_ready,
            phase(launch_failure ? PairwiseLifecyclePhase::worker_launch
                                 : PairwiseLifecyclePhase::worker_startup));
        team->cleanup_failures.clear();
        team->cleanup_failures.push_back(
            launch_failure ? std::move(*launch_failure)
                           : std::move(*startup_failure));
        shutdown_threads(*team, nullptr);
        return std::unexpected{PairwiseWorkerFailureSequence{
            .primary = std::move(primary),
            .cleanup = std::move(team->cleanup_failures)}};
    }
    for (auto& value : team->startup)
        team->readiness.push_back(std::move(value->value()));
    team->state = PairwisePersistentWorkerTeam::State::Phase::ready;
    return PairwisePersistentWorkerTeam{std::move(team)};
}

} // namespace epycbench::runtime::detail
