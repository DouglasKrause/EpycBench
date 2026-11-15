#include "runtime/frozen_observation_run.hpp"
#include "runtime/kernel_module_session.hpp"

#include "epycbench/kernel/registry.hpp"
#include "memory/pairwise_memory_access.hpp"
#include "runtime/pairwise_measurement_leaf_facade.hpp"
#include "runtime/pairwise_typed_production_binding_preparation.hpp"
#include "runtime/production_admission_policy.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <type_traits>

namespace epycbench::runtime::detail {
namespace {
auto add(std::uint64_t a, std::uint64_t b) noexcept -> std::optional<std::uint64_t> {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        return std::nullopt;
    return a + b;
}
auto multiply(std::uint64_t a, std::uint64_t b) noexcept -> std::optional<std::uint64_t> {
    if (a && b > std::numeric_limits<std::uint64_t>::max() / a)
        return std::nullopt;
    return a * b;
}
auto runtime_failure(PairwiseLifecyclePhase phase) noexcept -> PairwiseEvidenceFailure {
    auto value = PairwiseRuntimeFailure::create(
        PairwiseFailureCode::runtime_production_backend_invariant_failed,
        PairwiseFailureOperation::runtime_production_operations,
        PairwiseFailureReason::contained_exception, LifecyclePhaseContext{phase});
    if (!value)
        std::terminate();
    return std::move(*value);
}
auto callback_failure(PairwiseEvidenceFailure failure) noexcept -> PairwiseWorkerCallbackResult {
    auto value = PairwiseWorkerOperationFailure::create(std::move(failure));
    if (!value)
        std::terminate();
    return std::unexpected{PairwiseWorkerCallbackFailure{std::move(*value), std::nullopt}};
}
auto same_failure(const PairwiseEvidenceFailure& left,
                  const PairwiseEvidenceFailure& right) noexcept -> bool {
    return std::visit(
        [](const auto& a, const auto& b) noexcept {
            return a.code() == b.code() && a.operation() == b.operation() &&
                   a.reason() == b.reason() && a.context() == b.context() &&
                   a.posix_errno() == b.posix_errno();
        },
        left, right);
}
auto monitoring_evidence(const monitoring::MonitoringCaptureResult& capture) noexcept
    -> measurement::ObservationMonitoringEvidence {
    const auto signal = [](monitoring::MonitoringSignalState from) noexcept {
        using A = monitoring::MonitoringSignalState;
        using B = measurement::ObservationMonitoringSignalState;
        switch (from) {
        case A::clear:
            return B::clear;
        case A::detected:
            return B::detected;
        case A::unavailable:
            return B::unavailable;
        case A::not_applicable:
            return B::not_applicable;
        }
        std::terminate();
    };
    const auto& value = capture.evidence();
    return measurement::ObservationMonitoringEvidence::create(
        signal(value.page_faults_state()), signal(value.throttling_state()),
        signal(value.context_switches_state()), signal(value.frequency_state()),
        signal(value.temperature_state()));
}

using PartialMemory = std::variant<std::monostate, memory::RegionMapping, memory::BoundRegion,
                                   memory::InitializedRegion, memory::PlacementObservedRegion,
                                   memory::PlacementEvaluatedRegion>;
struct MemorySlot final {
    std::optional<memory::ReadOnlyRegion> owner;
    PartialMemory partial;
    std::optional<platform_linux::AnonymousMapping> failed_mapping;
    std::optional<PairwiseMemoryFinalizationFailure> failed_finalization;
    std::optional<kernel::PreparedSequentialReadBandwidth> bandwidth;
    std::optional<kernel::PreparedDependentLoadLatency> latency;
    std::optional<platform_linux::RawMonotonicTimestamp> start, end;
    std::optional<kernel::SequentialReadBandwidthWork> work;
    std::optional<kernel::SequentialReadBandwidthVerification> bandwidth_verification;
    std::optional<kernel::DependentLoadLatencySchedule> schedule;
    std::optional<kernel::DependentLoadLatencyVerification> latency_verification;
    bool timed_entered{};
    std::optional<memory::UnprotectedFinalizationReason> unprotected_reason;
};
struct Destination final {
    std::vector<measurement::SequentialReadBandwidthWorkerObservation> bandwidth;
    std::vector<measurement::DependentLoadLatencyWorkerObservation> latency;
    std::vector<measurement::WorkerPlacementValidityEvidence> placements;
    std::vector<measurement::WorkerMigrationValidityEvidence> migrations;
};
struct SignalGuard final {
    const PairwiseWorkerExecutionControl& control;
    bool signal_on_failure{true};
    bool sent{};
    void complete() noexcept {
        control.signal_timed_completion();
        sent = true;
    }
    ~SignalGuard() noexcept {
        if (!sent && signal_on_failure)
            control.signal_timed_completion();
    }
};
} // namespace

auto frozen_observation_failure_stock(std::uint64_t workers, std::uint64_t domains,
                                      std::uint64_t rollback, std::uint64_t completion,
                                      std::uint64_t stop, bool temperature, bool frequency) noexcept
    -> std::optional<FrozenObservationFailureStock> {
    if (!workers || !rollback || !completion || !stop)
        return std::nullopt;
    // Memory: preparation primary/cleanup + finalization primary/cleanup;
    // extra retained-cleanup and generation failures are charged separately.
    auto memory = multiply(workers, 6U);
    auto worker = multiply(workers, 2U);
    if (worker)
        worker = add(*worker, 4U); // primary plus 2W+3 forced shutdown stock
    auto source_pairs = multiply(domains, 2U);
    auto retirement = source_pairs ? add(*source_pairs, 4U) : std::nullopt;
    auto requested =
        frequency && source_pairs ? add(*source_pairs, 1U) : std::optional<std::uint64_t>{0U};
    if (requested)
        requested = add(*requested, temperature ? 3U : 0U);
    auto preparation = requested ? add(rollback, *requested) : std::nullopt;
    if (preparation && retirement)
        preparation = add(*preparation, *retirement);
    if (preparation)
        preparation = add(*preparation, 2U); // ordinary join + invariant
    auto capture = add(completion, 2U);      // primary and page-close disposition
    auto stopped = retirement ? add(stop, *retirement) : std::nullopt;
    if (stopped)
        stopped = add(*stopped, 1U);            // terminalization invariant
    constexpr std::uint64_t coordination = 12U; // three stages: begin/await/collect/abort
    if (!memory || !worker || !preparation || !capture || !stopped)
        return std::nullopt;
    std::uint64_t total = coordination;
    for (auto value : {*memory, *worker, *preparation, *capture, *stopped}) {
        auto next = add(total, value);
        if (!next)
            return std::nullopt;
        total = *next;
    }
    if (total > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return FrozenObservationFailureStock{
        static_cast<std::size_t>(*memory),      static_cast<std::size_t>(*worker),
        static_cast<std::size_t>(*preparation), static_cast<std::size_t>(*capture),
        static_cast<std::size_t>(*stopped),     static_cast<std::size_t>(coordination),
        static_cast<std::size_t>(total)};
}

struct FrozenObservationRunState final {
    enum class Phase : unsigned char { prepared, acquiring, ready, failed, retired };
    FrozenObservationRunState(const AdmittedPlacementRequest& placement_value,
                              FrozenObservationWork selected_value,
                              measurement::TimerOverheadEstimate overhead_value,
                              RunnerCancellation& cancel,
                              FrozenObservationRunDependencies dependency_values)
        : placement{placement_value}, selected{std::move(selected_value)}, overhead{overhead_value},
          cancellation{cancel}, dependencies{std::move(dependency_values)} {}
    const AdmittedPlacementRequest& placement;
    FrozenObservationWork selected;
    measurement::TimerOverheadEstimate overhead;
    RunnerCancellation& cancellation;
    FrozenObservationRunDependencies dependencies;
    std::vector<WorkerLaunchSpec> specs;
    std::vector<core::WorkerId> ids;
    std::vector<PairwisePreparedMemoryInput> memory;
    std::vector<MemorySlot> owners;
    std::vector<Destination> destinations;
    std::vector<FrozenObservationWorkerFinalization> finalizations;
    std::vector<PairwiseEvidenceFailure> failures;
    std::vector<FrozenObservationWorkerAssociation> terminal_workers;
    std::vector<ForcedTeamStopError> terminal_worker_force;
    std::optional<PairwisePreparedWorkerTeam> prepared_team;
    std::optional<PairwisePersistentWorkerTeam> team;
    std::optional<PairwisePreparedMonitoringStorage> prepared_monitoring;
    std::optional<PairwiseMonitoringOwningFacade> monitoring;
    std::optional<PairwisePreparedRegionInitializer> initializer;
    std::optional<kernel::PreparedDependentLoadLatencyVerification> verification;
    std::optional<measurement::ObservationValidityPolicy> validity;
    std::optional<kernel::SequentialReadBandwidthConfig> bandwidth_configuration;
    std::optional<kernel::DependentLoadLatencyConfig> latency_configuration;
    kernel::KernelImplementationId implementation{};
    std::uint64_t latency_seed{};
    std::size_t next{};
    Phase phase{Phase::prepared};
    bool cleanup_confirmed{true};
    bool memory_retired{};
    bool test_terminal_failures_consumed{};
    bool monitoring_retired{};
    bool workers_retired{};
    std::array<PairwiseMonitoringForceDisposition, 3U> service_force{};
    std::size_t service_force_count{};

    void append(PairwiseEvidenceFailure value) noexcept {
        // Identical to terminal stop: a consumed perf-retirement failure leaves
        // cleanup uncertain, including rollback before the Monitoring owner exists.
        if (std::visit([](const auto& failure) noexcept {
                return failure.operation() == PairwiseFailureOperation::retire_perf_context_switch;
            }, value))
            cleanup_confirmed = false;
        if (failures.size() == failures.capacity())
            std::terminate();
        failures.push_back(std::move(value));
    }
    void append_distinct(PairwiseEvidenceFailure value, std::size_t begin) noexcept {
        for (std::size_t index = begin; index < failures.size(); ++index)
            if (same_failure(failures[index], value))
                return;
        append(std::move(value));
    }
    template <class Cursor> void consume(Cursor cursor) noexcept {
        while (!cursor.empty())
            append(cursor.next());
    }
    void source_dispositions(
        std::span<const platform_linux::detail::BoundedSysfsDisposition> values) noexcept {
        for (const auto& value : values)
            if (value.kind != platform_linux::detail::BoundedSysfsDispositionKind::success)
                cleanup_confirmed = false;
    }
    void monitoring_preparation_failure(PairwiseMonitoringPreparationFailure failure) noexcept {
        source_dispositions(failure.source_retirement());
        service_force_count = failure.force_disposition_count();
        for (std::size_t index{}; index < service_force_count; ++index)
            service_force[index] = *failure.force_disposition(index);
        consume(PairwiseMonitoringFailureCursor::consume(std::move(failure)));
    }
    void capture_failure(PairwiseMonitoringCaptureFailure failure) noexcept {
        if (failure.page_close_disposition() && failure.page_close_disposition()->close_failed)
            cleanup_confirmed = false;
        consume(PairwiseMonitoringCaptureFailureCursor::consume(std::move(failure)));
    }
    auto clock(PairwiseTypedClockSite site,
               std::optional<core::WorkerId> worker = std::nullopt) noexcept
        -> std::expected<platform_linux::RawMonotonicTimestamp, PairwisePlatformFailure> {
        const auto& hooks = dependencies.workers.hooks;
        if (hooks.before_clock)
            hooks.before_clock(hooks.context, site, true, worker);
        return dependencies.platform.raw_monotonic_now();
    }
    auto command_contains(const PairwiseWorkerCommandOutcome& outcome,
                          const PairwiseEvidenceFailure& failure) const noexcept -> bool {
        return std::any_of(outcome.workers().begin(), outcome.workers().end(),
                           [&](const auto& worker) noexcept {
                               return (worker.failure && same_failure(*worker.failure, failure)) ||
                                      (worker.operation_primary &&
                                       same_failure(*worker.operation_primary, failure));
                           });
    }
    void command_failures(const PairwiseWorkerCommandOutcome& outcome) noexcept {
        // The original nonpeer failure precedes stopped-peer consequences even
        // when that peer has a lower canonical WorkerId. Retain every actual
        // occurrence; only its enclosing operation wrapper is redundant.
        for (const bool peers : {false, true}) for (const auto& worker : outcome.workers()) {
            if (worker.peer_stopped != peers) continue;
            if (worker.operation_primary) append(*worker.operation_primary);
            if (worker.operation_cleanup) append(*worker.operation_cleanup);
            if (worker.failure && (!worker.operation_primary ||
                worker.failure->code() != PairwiseFailureCode::worker_command_failed ||
                worker.failure->reason() != PairwiseFailureReason::worker_operation_failed))
                append(*worker.failure);
        }
    }
    void retire() noexcept;
    ~FrozenObservationRunState() noexcept { retire(); }
};

namespace {
auto prepare_memory(void* raw, const WorkerExecutionContext& worker,
                    const PairwiseWorkerExecutionControl& control) noexcept
    -> PairwiseWorkerCallbackResult {
    SignalGuard signal{control};
    auto& state = *static_cast<FrozenObservationRunState*>(raw);
    auto& input = state.memory[worker.worker_index];
    auto& slot = state.owners[worker.worker_index];
    PairwiseMemoryOwningFacade memory{state.dependencies.platform, state.dependencies.kernel,
                                      state.dependencies.memory_hooks};
    auto mapped = memory.map(std::move(input.request));
    if (!mapped) {
        auto failure = std::move(mapped.error());
        if (failure.retained_owner)
            slot.failed_mapping.emplace(std::move(*failure.retained_owner));
        if (failure.cleanup) {
            auto primary =
                PairwiseWorkerOperationFailure::create(PairwiseEvidenceFailure{failure.primary});
            auto cleanup =
                PairwiseWorkerOperationFailure::create(PairwiseEvidenceFailure{*failure.cleanup});
            if (!primary || !cleanup)
                std::terminate();
            return std::unexpected{
                PairwiseWorkerCallbackFailure{std::move(*primary), std::move(*cleanup)}};
        }
        return callback_failure(std::move(failure.primary));
    }
    auto bound = memory.bind(std::move(*mapped), input.workspace);
    if (!bound) {
        slot.partial.emplace<memory::RegionMapping>(std::move(bound.error().owner));
        return callback_failure(std::move(bound.error().primary));
    }
    auto initialized = memory.initialize(std::move(*bound), &*state.initializer);
    if (!initialized) {
        slot.partial.emplace<memory::BoundRegion>(std::move(initialized.error().owner));
        return std::visit(
            [](auto&& value) {
                return callback_failure(PairwiseEvidenceFailure{std::move(value)});
            },
            std::move(initialized.error().primary));
    }
    auto observed = memory.observe(std::move(*initialized), input.workspace);
    if (!observed) {
        slot.partial.emplace<memory::InitializedRegion>(std::move(observed.error().owner));
        return callback_failure(std::move(observed.error().primary));
    }
    auto evaluated = memory.evaluate(std::move(*observed));
    if (!evaluated) {
        slot.partial.emplace<memory::PlacementObservedRegion>(std::move(evaluated.error().owner));
        return callback_failure(std::move(evaluated.error().primary));
    }
    auto protected_region = memory.protect(std::move(*evaluated));
    if (!protected_region) {
        slot.unprotected_reason = protected_region.error().primary.reason() ==
                                          PairwiseFailureReason::execution_not_eligible
                                      ? memory::UnprotectedFinalizationReason::execution_ineligible
                                      : memory::UnprotectedFinalizationReason::protection_failed;
        slot.partial.emplace<memory::PlacementEvaluatedRegion>(
            std::move(protected_region.error().owner));
        return callback_failure(std::move(protected_region.error().primary));
    }
    slot.owner.emplace(std::move(*protected_region));
    if (state.bandwidth_configuration) {
        auto input_kernel = state.dependencies.kernel.bandwidth_input(
            *state.bandwidth_configuration, slot.owner->requested_region());
        if (!input_kernel)
            return callback_failure(std::move(input_kernel.error()));
        slot.bandwidth.emplace(
            kernel::PreparedSequentialReadBandwidth::prepare(std::move(*input_kernel)));
        if (!bind_kernel_module(*slot.bandwidth, state.implementation))
            return callback_failure(runtime_failure(PairwiseLifecyclePhase::preparation));
    } else {
        auto input_kernel = state.dependencies.kernel.latency_input(*state.latency_configuration,
                                                                    slot.owner->requested_region());
        if (!input_kernel)
            return callback_failure(std::move(input_kernel.error()));
        slot.latency.emplace(
            kernel::PreparedDependentLoadLatency::prepare(std::move(*input_kernel)));
        if (!bind_kernel_module(*slot.latency, state.implementation))
            return callback_failure(runtime_failure(PairwiseLifecyclePhase::preparation));
    }
    return {};
}

struct ObservationCommand final {
    FrozenObservationRunState& state;
    core::ExperimentId experiment;
    kernel::DependentLoadLatencyBlockId block;
};
auto run_kernel(void* raw, const WorkerExecutionContext& worker,
                const PairwiseWorkerExecutionControl& control) noexcept
    -> PairwiseWorkerCallbackResult {
    SignalGuard signal{control, false};
    auto& command = *static_cast<ObservationCommand*>(raw);
    auto& state = command.state;
    auto& slot = state.owners[worker.worker_index];
    if (control.runner_cancellation.requested() || control.team_stop.stop_requested() ||
        control.command_stop.stop_requested()) {
        // The owning worker facade records the actual control failure. A worker
        // control tuple is not a lower-operation capability and cannot be nested.
        return {};
    }
    if (state.bandwidth_configuration) {
        auto start = state.clock(PairwiseTypedClockSite::worker_start, worker.worker_id);
        if (!start)
            return callback_failure(std::move(start.error()));
        slot.start.emplace(*start);
        if (control.runner_cancellation.requested() || control.team_stop.stop_requested() ||
            control.command_stop.stop_requested()) return {};
        const auto passes = std::get<kernel::SequentialReadBandwidthPassCount>(state.selected);
        slot.timed_entered = true;
        const auto terminal = kernel::execute_sequential_read_bandwidth(*slot.bandwidth, passes);
        auto end = state.clock(PairwiseTypedClockSite::worker_end, worker.worker_id);
        if (!end)
            return callback_failure(std::move(end.error()));
        slot.end.emplace(*end);
        signal.complete();
        auto accounted = state.dependencies.kernel.bandwidth_work(*slot.bandwidth, passes);
        if (!accounted)
            return callback_failure(std::move(accounted.error()));
        slot.work.emplace(*accounted);
        const auto checksum =
            kernel::PreparedSequentialReadBandwidthChecksum::prepare(*slot.bandwidth);
        slot.bandwidth_verification.emplace(kernel::verify_sequential_read_bandwidth_checksum(
            checksum.expected_terminal(passes), terminal));
        if (!module_verification_matches(*slot.bandwidth, *slot.bandwidth_verification))
            return callback_failure(runtime_failure(PairwiseLifecyclePhase::active));
    } else {
        const auto steps = std::get<kernel::DependentLoadLatencyStepCount>(state.selected);
        auto seed = state.dependencies.kernel.prepare_latency_schedule_seed(
            state.latency_seed, command.experiment, command.block);
        if (!seed)
            return callback_failure(std::move(seed.error()));
        auto schedule = state.dependencies.kernel.latency_schedule(*slot.latency, *seed, steps);
        if (!schedule)
            return callback_failure(std::move(schedule.error()));
        slot.schedule.emplace(*schedule);
        auto start = state.clock(PairwiseTypedClockSite::worker_start, worker.worker_id);
        if (!start)
            return callback_failure(std::move(start.error()));
        slot.start.emplace(*start);
        if (control.runner_cancellation.requested() || control.team_stop.stop_requested() ||
            control.command_stop.stop_requested()) return {};
        slot.timed_entered = true;
        const auto terminal = kernel::execute_dependent_load_latency(*slot.latency, *schedule);
        auto end = state.clock(PairwiseTypedClockSite::worker_end, worker.worker_id);
        if (!end)
            return callback_failure(std::move(end.error()));
        slot.end.emplace(*end);
        signal.complete();
        slot.latency_verification.emplace(kernel::verify_dependent_load_latency_terminal(
            state.verification->expected_terminal(*schedule), terminal));
        if (!module_verification_matches(*slot.latency, *slot.latency_verification))
            return callback_failure(runtime_failure(PairwiseLifecyclePhase::active));
    }
    return {};
}
} // namespace

void FrozenObservationRunState::retire() noexcept {
    if (phase == Phase::retired)
        return;
    if (phase == Phase::prepared) {
        phase = Phase::retired;
        return;
    }
    if (!memory_retired) {
        PairwiseMemoryOwningFacade facade{dependencies.platform, dependencies.kernel,
                                          dependencies.memory_hooks};
        for (std::size_t index{}; index < owners.size(); ++index) {
            auto& slot = owners[index];
            auto& output = finalizations[index];
            output.timed_entered = slot.timed_entered;
            slot.bandwidth.reset();
            slot.latency.reset();
            const auto retain_finalization =
                [&](PairwiseMemoryFinalizationFailure failure) noexcept {
                    append(failure.primary);
                    if (failure.cleanup)
                        append(*failure.cleanup);
                    slot.failed_finalization.emplace(std::move(failure));
                    auto& retained = *slot.failed_finalization;
                    // This existing rvalue-reference seam moves only factual evidence;
                    // the native mapping stays owned. The remaining typed owner is never
                    // supplied to another consuming Memory operation.
                    output.diagnostic.emplace(
                        memory::detail::RegionFinalizationAccess::take_failure_diagnostic(
                            std::move(retained.owner), std::move(retained.post_run),
                            retained.cleanup_evidence, retained.path));
                    output.ownership_remains = retained.cleanup_evidence.mapping_active_after();
                    if (output.ownership_remains)
                        cleanup_confirmed = false;
                };
            if (slot.owner) {
                auto finalized = facade.finalize(
                    std::move(*slot.owner),
                    slot.timed_entered ? memory::PostRunVerificationApplicability::applicable
                                       : memory::PostRunVerificationApplicability::not_applicable,
                    memory[index].workspace);
                slot.owner.reset();
                if (finalized)
                    output.diagnostic = memory::detail::RegionFinalizationAccess::take_diagnostic(
                        std::move(*finalized));
                else
                    retain_finalization(std::move(finalized.error()));
            } else {
                std::visit(
                    [&](auto& retained) noexcept {
                        using Owner = std::remove_cvref_t<decltype(retained)>;
                        if constexpr (std::same_as<Owner, std::monostate>)
                            return;
                        else if constexpr (std::same_as<Owner, memory::PlacementEvaluatedRegion>) {
                            auto finalized = facade.finalize_unprotected(std::move(retained),
                                                                         *slot.unprotected_reason);
                            if (finalized)
                                output.diagnostic =
                                    memory::detail::RegionFinalizationAccess::take_diagnostic(
                                        std::move(*finalized));
                            else
                                retain_finalization(std::move(finalized.error()));
                        } else {
                            auto cleaned = facade.cleanup(std::move(retained));
                            if (cleaned)
                                output.partial_cleanup.emplace(cleaned->cleanup_evidence());
                            else {
                                auto failure = std::move(cleaned.error());
                                append(failure.primary);
                                output.partial_cleanup.emplace(failure.evidence);
                                output.ownership_remains = failure.evidence.mapping_active_after();
                                retained = std::move(failure.owner);
                                if (output.ownership_remains)
                                    cleanup_confirmed = false;
                            }
                        }
                    },
                    slot.partial);
            }
            if (slot.failed_mapping && slot.failed_mapping->active()) {
                output.ownership_remains = true;
                cleanup_confirmed = false;
            }
        }
        memory_retired = true;
    }
    if (monitoring && !monitoring_retired) {
        auto outcome = monitoring->stop_and_retire(dependencies.platform);
        service_force_count = outcome.force_disposition_count();
        for (std::size_t index{}; index < service_force_count; ++index)
            service_force[index] = *outcome.force_disposition(index);
        if (outcome.invariant_failed() || monitoring->has_service_owner())
            cleanup_confirmed = false;
        source_dispositions(outcome.source_retirement());
        consume(PairwiseMonitoringFailureCursor::consume(std::move(outcome)));
        monitoring_retired = true;
    }
    if (team && !workers_retired) {
        auto stopped = team->stop_and_join();
        if (!stopped) {
            append(stopped.error().primary);
            for (auto& error : stopped.error().cleanup)
                append(std::move(error));
        }
        if (team->active())
            cleanup_confirmed = false;
        workers_retired = true;
    }
    phase = Phase::retired;
}

FrozenObservationRun::FrozenObservationRun(
    std::unique_ptr<FrozenObservationRunState> state) noexcept
    : state_{std::move(state)} {}
FrozenObservationRun::FrozenObservationRun(FrozenObservationRun&&) noexcept = default;
FrozenObservationRun::~FrozenObservationRun() noexcept = default;
FrozenObservationRetirement::FrozenObservationRetirement(
    std::unique_ptr<FrozenObservationRunState> state) noexcept
    : state_{std::move(state)} {}
FrozenObservationRetirement::FrozenObservationRetirement(FrozenObservationRetirement&&) noexcept =
    default;
FrozenObservationRetirement::~FrozenObservationRetirement() noexcept = default;
auto FrozenObservationRetirement::failures() const noexcept
    -> std::span<const PairwiseEvidenceFailure> {
    return state_ ? std::span<const PairwiseEvidenceFailure>{state_->failures}
                  : std::span<const PairwiseEvidenceFailure>{};
}
auto FrozenObservationRetirement::workers() const noexcept
    -> std::span<const FrozenObservationWorkerFinalization> {
    return state_ ? std::span<const FrozenObservationWorkerFinalization>{state_->finalizations}
                  : std::span<const FrozenObservationWorkerFinalization>{};
}
auto FrozenObservationRetirement::cleanup_confirmed() const noexcept -> bool {
    return state_ && state_->cleanup_confirmed;
}
auto FrozenObservationRetirement::monitoring_force_dispositions() const noexcept
    -> std::span<const PairwiseMonitoringForceDisposition> {
    return state_ ? std::span{state_->service_force}.first(state_->service_force_count)
                  : std::span<const PairwiseMonitoringForceDisposition>{};
}
auto FrozenObservationRetirement::worker_force_errors() const noexcept
    -> std::span<const ForcedTeamStopError> {
    return state_ && state_->team ? PairwiseWorkerOwningTestAccess::forced_errors(*state_->team)
                                  : std::span<const ForcedTeamStopError>{};
}
auto frozen_observation_terminal_stock(std::uint64_t workers) noexcept
    -> std::optional<FrozenObservationTerminalStock> {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (!workers || workers > (maximum - 3U) / 2U) return std::nullopt;
    return FrozenObservationTerminalStock{static_cast<std::size_t>(workers),
        static_cast<std::size_t>(workers) * 2U + 3U};
}
auto FrozenObservationRetirement::preparation_facts() const noexcept
    -> std::optional<MonitoringPreparationFactView> {
    return state_ && state_->monitoring ? state_->monitoring->retired_preparation_facts()
                                       : std::nullopt;
}
auto FrozenObservationTerminalStockTestAccess::replace_confirmed_stock(
    FrozenObservationRetirement& retirement,
    std::vector<FrozenObservationWorkerAssociation>&& workers,
    std::vector<ForcedTeamStopError>&& forced) noexcept -> bool {
    if (!retirement.state_ || !retirement.state_->cleanup_confirmed || !workers.empty() ||
        !forced.empty() || !retirement.state_->terminal_workers.empty() ||
        !retirement.state_->terminal_worker_force.empty()) return false;
    retirement.state_->terminal_workers.swap(workers);
    retirement.state_->terminal_worker_force.swap(forced);
    return true;
}
auto FrozenObservationTerminalStockTestAccess::consume_confirmed_command_failures(
    FrozenObservationRetirement& retirement, const PairwiseWorkerCommandOutcome& outcome) noexcept -> bool {
    if (!retirement.state_ || !retirement.state_->cleanup_confirmed ||
        retirement.state_->test_terminal_failures_consumed ||
        outcome.workers().size() != retirement.state_->ids.size()) return false;
    const auto remaining = retirement.state_->failures.capacity() - retirement.state_->failures.size();
    std::size_t count{};
    for (std::size_t i{}; i < outcome.workers().size(); ++i) {
        const auto& worker = outcome.workers()[i];
        if (worker.worker_id != retirement.state_->ids[i]) return false;
        auto additions = static_cast<std::size_t>(worker.operation_primary.has_value()) +
                         static_cast<std::size_t>(worker.operation_cleanup.has_value());
        if (worker.failure && (!worker.operation_primary ||
            worker.failure->code() != PairwiseFailureCode::worker_command_failed ||
            worker.failure->reason() != PairwiseFailureReason::worker_operation_failed)) ++additions;
        if (additions > remaining - count) return false;
        count += additions;
    }
    retirement.state_->test_terminal_failures_consumed = true;
    retirement.state_->command_failures(outcome);
    return true;
}
auto FrozenObservationRetirement::detach_confirmed(
    const monitoring::MonitoringCaptureResult* capture) && noexcept
    -> std::expected<FrozenObservationTerminalFacts, FrozenObservationRunReason> {
    if (!state_) return std::unexpected{FrozenObservationRunReason::unavailable};
    if (!state_->cleanup_confirmed)
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    auto view = preparation_facts();
    const auto forced = worker_force_errors();
    if ((view && view->workers().size() > state_->terminal_workers.capacity()) ||
        forced.size() > state_->terminal_worker_force.capacity())
        return std::unexpected{FrozenObservationRunReason::capacity};
    auto metadata = capture ? FrozenObservationMetadataComparison::not_requested
                            : FrozenObservationMetadataComparison::no_completed_capture;
    std::optional<FrozenObservationPreparationFacts> preparation;
    if (view) {
        for (const auto& worker : view->workers())
            state_->terminal_workers.push_back({worker.worker_id(), worker.linux_cpu()});
        if (capture && view->frequency_domains()) {
            metadata = FrozenObservationMetadataComparison::matched;
            const auto raw = capture->raw().frequency_metadata();
            if (raw.size() != view->frequency_domains())
                metadata = FrozenObservationMetadataComparison::mismatch;
            else for (std::size_t i{}; i < raw.size(); ++i) {
                const auto actual = view->frequency(i);
                const auto& row = raw[i];
                if (!actual || !std::ranges::equal(actual->cpus, row.domain_id().cpus()) ||
                    actual->kind != row.source_kind() ||
                    actual->driver != row.scaling_driver().value() ||
                    actual->governor != row.scaling_governor().value() ||
                    actual->minimum_hz != row.scaling_minimum().value() ||
                    actual->maximum_hz != row.scaling_maximum().value())
                    metadata = FrozenObservationMetadataComparison::mismatch;
            }
        } else if (capture && !capture->raw().frequency_metadata().empty()) {
            metadata = FrozenObservationMetadataComparison::mismatch;
        }
        preparation.emplace(FrozenObservationPreparationFacts{
            view->domain(), view->workers().size(), view->frequency_domains(),
            view->channels(), view->service(), std::move(state_->terminal_workers)});
    }
    for (const auto& error : forced) state_->terminal_worker_force.push_back(error);
    FrozenObservationTerminalFacts result{
        std::move(state_->failures), std::move(state_->finalizations), std::move(preparation),
        metadata, state_->service_force, state_->service_force_count,
        std::move(state_->terminal_worker_force)};
    state_.reset();
    return result;
}
auto FrozenObservationRun::ready() const noexcept -> bool {
    return state_ && state_->phase == FrozenObservationRunState::Phase::ready;
}
auto FrozenObservationRun::failures() const noexcept -> std::span<const PairwiseEvidenceFailure> {
    return state_ ? std::span<const PairwiseEvidenceFailure>{state_->failures}
                  : std::span<const PairwiseEvidenceFailure>{};
}
auto FrozenObservationRun::failure_capacity() const noexcept -> std::size_t {
    return state_ ? state_->failures.capacity() : 0U;
}

auto FrozenObservationRun::prepare(const AdmittedPlacementRequest& placement,
                                   const monitoring::MonitoringExecutionPolicy& policy,
                                   const ServicePlacementPlan& services,
                                   PairwiseAdmittedMonitoringCapacity capacity,
                                   FrozenObservationWork work, std::size_t observations,
                                   measurement::TimerOverheadEstimate overhead,
                                   RunnerCancellation& cancellation,
                                   FrozenObservationRunDependencies dependencies) noexcept
    -> std::expected<FrozenObservationRun, FrozenObservationRunReason> {
    const auto count = placement.workers().size();
    if (!count || count > 8U || placement.execution_inputs().measurement().calibration_enabled() ||
        count != capacity.workers() || policy != capacity.admission().monitoring_policy())
        return std::unexpected{FrozenObservationRunReason::invalid_input};
    const auto* bandwidth =
        std::get_if<ipc::PlacementBandwidthExecutionInputs>(&placement.execution_inputs().kernel());
    const auto* latency =
        std::get_if<ipc::PlacementLatencyExecutionInputs>(&placement.execution_inputs().kernel());
    if ((bandwidth != nullptr) !=
            std::holds_alternative<kernel::SequentialReadBandwidthPassCount>(work) ||
        (latency && count != 1U))
        return std::unexpected{FrozenObservationRunReason::wrong_family};
    if (!observations || observations > capacity.capture_slots())
        return std::unexpected{FrozenObservationRunReason::capacity};
    auto stock = frozen_observation_failure_stock(
        capacity.workers(), capacity.maximum_domain_count(), capacity.preparation_rollback_values(),
        capacity.completion_abort_values(), capacity.whole_stop_values(),
        capacity.temperature_requested(), capacity.frequency_requested());
    if (!stock)
        return std::unexpected{FrozenObservationRunReason::capacity};
    if (bandwidth) {
        auto bytes = multiply(bandwidth->configuration().working_set_per_worker().value(),
                              std::get<kernel::SequentialReadBandwidthPassCount>(work).value());
        if (!bytes || !multiply(*bytes, count))
            return std::unexpected{FrozenObservationRunReason::capacity};
    }
    try {
        if (!PairwiseMonitoringHostCapacityAdmissionAccess::matches_single(
                capacity.admission(), placement, policy, services))
            return std::unexpected{FrozenObservationRunReason::invalid_input};
        auto state = std::make_unique<FrozenObservationRunState>(
            placement, std::move(work), overhead, cancellation, std::move(dependencies));
        state->failures.reserve(stock->total);
        const auto terminal_stock = frozen_observation_terminal_stock(count);
        if (!terminal_stock) return std::unexpected{FrozenObservationRunReason::capacity};
        state->terminal_workers.reserve(terminal_stock->workers);
        state->terminal_worker_force.reserve(terminal_stock->forced_worker_errors);
        state->specs = prepare_pairwise_worker_specs(placement);
        state->ids.reserve(count);
        for (const auto& spec : state->specs)
            state->ids.push_back(spec.worker_id);
        state->owners.resize(count);
        state->finalizations.reserve(count);
        for (const auto& id : state->ids)
            state->finalizations.push_back({id, false, {}, {}, false});
        state->destinations.resize(observations);
        for (auto& destination : state->destinations) {
            if (bandwidth)
                destination.bandwidth.reserve(count);
            else
                destination.latency.reserve(count);
            destination.placements.reserve(count);
            destination.migrations.reserve(count);
        }
        auto source = state->dependencies.sources;
        source.present_cpus = capacity.admission().authority().present_cpus();
        source.worker_cpus = capacity.admission().worker_cpus();
        if (state->dependencies.source_capacity_from_admission) {
            // Reuse the production binding's dimensions. These borrows end
            // inside prepare_capacity_storage while capacity is still owned;
            // no Session projection or duplicate CPU buffer is required.
            const auto maximum_cpu = static_cast<std::uint64_t>(
                capacity.admission().authority().kernel_maximum_cpu());
            source.temperature_capacity = {capacity.present_cpu_count(),
                capacity.enumeration_capacity(), capacity.base_page_bytes()};
            source.frequency_capacity = {maximum_cpu, maximum_cpu + 2U,
                maximum_cpu + 1U, capacity.maximum_membership_count(),
                capacity.base_page_bytes()};
        }
        auto monitoring = PairwiseMonitoringOwningFacade::prepare_capacity_storage(
            std::move(capacity), state->dependencies.platform, source);
        if (!monitoring)
            return std::unexpected{FrozenObservationRunReason::operational_failure};
        state->prepared_monitoring.emplace(std::move(*monitoring));
        PairwiseMeasurementLeafFacade measurement;
        const auto& input = placement.execution_inputs().measurement();
        auto maximum =
            measurement.maximum_harness_overhead(input.maximum_harness_overhead().numerator(),
                                                 input.maximum_harness_overhead().denominator());
        if (!maximum)
            return std::unexpected{FrozenObservationRunReason::invalid_input};
        state->validity.emplace(measurement::ObservationValidityPolicy::create(*maximum));
        auto threshold = measurement.warmup_stability_threshold(1U, 10U);
        if (!threshold)
            return std::unexpected{FrozenObservationRunReason::invalid_input};
        auto warmup = measurement.warmup_policy(0U, 0U, 0U, *threshold);
        const auto blocks =
            static_cast<std::uint64_t>(observations / 4U + (observations % 4U != 0U ? 1U : 0U));
        auto repetition = measurement.repetition_policy(0U, blocks, 0U, false);
        if (!warmup || !repetition)
            return std::unexpected{FrozenObservationRunReason::invalid_input};
        auto team = PairwiseWorkerOwningFacade::prepare(
            state->specs, cancellation, *warmup, *repetition, state->dependencies.workers.platform);
        if (!team)
            return std::unexpected{FrozenObservationRunReason::operational_failure};
        state->prepared_team.emplace(std::move(*team));
        PairwiseMemoryOwningFacade memory{state->dependencies.platform, state->dependencies.kernel,
                                          state->dependencies.memory_hooks};
        auto memory_inputs = prepare_pairwise_memory_inputs(placement, memory);
        if (!memory_inputs)
            return std::unexpected{FrozenObservationRunReason::operational_failure};
        state->memory = std::move(*memory_inputs);
        const auto* descriptor =
            kernel::find_kernel_descriptor(placement.request().experiment().kernel_id());
        if (!descriptor)
            return std::unexpected{FrozenObservationRunReason::invalid_input};
        state->implementation = descriptor->implementation_id();
        if (bandwidth) {
            state->bandwidth_configuration.emplace(bandwidth->configuration());
            auto initializer = state->dependencies.kernel.prepare_region_initializer(
                kernel::RegionInitializationSpec::bandwidth(bandwidth->configuration()));
            if (!initializer)
                return std::unexpected{FrozenObservationRunReason::operational_failure};
            state->initializer.emplace(std::move(*initializer));
        } else {
            state->latency_configuration.emplace(latency->configuration());
            state->latency_seed = latency->resolved_permutation_seed();
            auto specification = state->dependencies.kernel.latency_region_spec(
                latency->configuration(), state->latency_seed);
            if (!specification)
                return std::unexpected{FrozenObservationRunReason::operational_failure};
            auto initializer =
                state->dependencies.kernel.prepare_region_initializer(*specification);
            if (!initializer)
                return std::unexpected{FrozenObservationRunReason::operational_failure};
            state->initializer.emplace(std::move(*initializer));
            auto verification = state->dependencies.kernel.latency_verification_for_configuration(
                latency->configuration(), state->latency_seed);
            if (!verification)
                return std::unexpected{FrozenObservationRunReason::operational_failure};
            state->verification.emplace(std::move(*verification));
        }
        return FrozenObservationRun{std::move(state)};
    } catch (const std::bad_alloc&) {
        return std::unexpected{FrozenObservationRunReason::allocation};
    } catch (...) {
        return std::unexpected{FrozenObservationRunReason::contained_exception};
    }
}

auto FrozenObservationRun::activate() noexcept -> std::expected<void, FrozenObservationRunReason> {
    if (!state_ || state_->phase != FrozenObservationRunState::Phase::prepared)
        return std::unexpected{FrozenObservationRunReason::unavailable};
    auto& state = *state_;
    state.phase = FrozenObservationRunState::Phase::acquiring;
    const auto failed = [&]() -> std::expected<void, FrozenObservationRunReason> {
        state.phase = FrozenObservationRunState::Phase::failed;
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    };
    if (state.cancellation.requested()) {
        state.phase = FrozenObservationRunState::Phase::failed;
        return std::unexpected{FrozenObservationRunReason::cancelled};
    }
    auto team = PairwiseWorkerOwningFacade::start(std::move(*state.prepared_team),
                                                  state.dependencies.workers);
    state.prepared_team.reset();
    if (!team) {
        state.append(team.error().primary);
        for (auto& error : team.error().cleanup)
            state.append(std::move(error));
        return failed();
    }
    state.team.emplace(std::move(*team));
    auto generation = state.team->begin_generation(
        PairwiseWorkerGenerationClass::memory_preparation, {&state, prepare_memory},
        WorkerObservationPolicy::none, WorkerCancellationPolicy::cleanup_insensitive);
    if (!generation) {
        state.append(generation.error());
        return failed();
    }
    auto ready = generation->await_ready();
    if (!ready) {
        state.append(ready.error());
        generation->cancel_before_release();
        auto quiescent = generation->await_quiescence();
        if (!quiescent)
            state.append_distinct(quiescent.error(), 0U);
        return failed();
    }
    generation->release();
    auto timed = generation->await_timed_completion();
    auto quiescent = generation->await_quiescence();
    auto outcome = generation->collect_ordered_results();
    if (outcome) {
        state.command_failures(*outcome);
        if (!timed && !state.command_contains(*outcome, timed.error()))
            state.append(timed.error());
        if (!quiescent && (timed || !same_failure(timed.error(), quiescent.error())) &&
            !state.command_contains(*outcome, quiescent.error()))
            state.append(quiescent.error());
    } else {
        // Projection cannot recover the actual worker occurrence. Preserve the
        // earlier wait failure before the later consuming-projection failure.
        if (!timed) state.append(timed.error());
        if (!quiescent && (timed || !same_failure(timed.error(), quiescent.error())))
            state.append(quiescent.error());
        if ((timed || !same_failure(timed.error(), outcome.error())) &&
            (quiescent || !same_failure(quiescent.error(), outcome.error())))
            state.append(outcome.error());
    }
    if (!state.failures.empty())
        return failed();
    auto readiness = state.team->lend_monitoring_readiness();
    if (!readiness) {
        state.append(runtime_failure(PairwiseLifecyclePhase::preparation));
        return failed();
    }
    auto bound = std::move(*state.prepared_monitoring).bind_worker_readiness(std::move(*readiness));
    state.prepared_monitoring.reset();
    if (!bound) {
        state.monitoring_preparation_failure(std::move(bound.error()));
        return failed();
    }
    auto monitoring = std::move(*bound).acquire_sources_and_owner(state.dependencies.platform);
    if (!monitoring) {
        state.monitoring_preparation_failure(std::move(monitoring.error()));
        return failed();
    }
    state.monitoring.emplace(std::move(*monitoring));
    if (state.cancellation.requested()) {
        state.phase = FrozenObservationRunState::Phase::failed;
        return std::unexpected{FrozenObservationRunReason::cancelled};
    }
    state.phase = FrozenObservationRunState::Phase::ready;
    return {};
}

auto FrozenObservationRun::observe_bandwidth(std::size_t ordinal, core::ObservationId observation,
                                             core::ExperimentId experiment,
                                             kernel::DependentLoadLatencyBlockId block) noexcept
    -> std::expected<FrozenObservation, FrozenObservationRunReason> {
    return observe(ordinal, observation, experiment, block, true);
}
auto FrozenObservationRun::observe_latency(std::size_t ordinal, core::ObservationId observation,
                                           core::ExperimentId experiment,
                                           kernel::DependentLoadLatencyBlockId block) noexcept
    -> std::expected<FrozenObservation, FrozenObservationRunReason> {
    return observe(ordinal, observation, experiment, block, false);
}
auto FrozenObservationRun::observe(std::size_t ordinal, core::ObservationId observation_id,
                                   core::ExperimentId experiment,
                                   kernel::DependentLoadLatencyBlockId block,
                                   bool bandwidth) noexcept
    -> std::expected<FrozenObservation, FrozenObservationRunReason> {
    if (!ready())
        return std::unexpected{FrozenObservationRunReason::unavailable};
    auto& state = *state_;
    if (bandwidth != state.bandwidth_configuration.has_value())
        return std::unexpected{FrozenObservationRunReason::wrong_family};
    if (ordinal != state.next || ordinal >= state.destinations.size())
        return std::unexpected{FrozenObservationRunReason::ordinal};
    ++state.next; // failure also permanently consumes this prepared destination
    state.phase = FrozenObservationRunState::Phase::failed;
    if (state.cancellation.requested())
        return std::unexpected{FrozenObservationRunReason::cancelled};
    ObservationCommand command{state, experiment, block};
    auto generation = state.team->begin_generation(
        PairwiseWorkerGenerationClass::observation, {&command, run_kernel},
        WorkerObservationPolicy::before_and_after, WorkerCancellationPolicy::observe);
    if (!generation) {
        state.append(generation.error());
        return std::unexpected{generation.error().reason() == PairwiseFailureReason::runner_cancelled
            ? FrozenObservationRunReason::cancelled : FrozenObservationRunReason::operational_failure};
    }
    const auto start_failures = state.failures.size();
    const auto quiesce_cleanup = [&](const PairwiseEvidenceFailure* awaited = nullptr,
                                     bool awaited_recorded = false) noexcept {
        auto quiescent = generation->await_quiescence();
        // Runtime command results are consumed solely for exact cleanup chronology;
        // no MSR006 factual worker Observation is collected on these branches.
        auto outcome = generation->collect_ordered_results();
        if (outcome) {
            state.command_failures(*outcome);
            // Keep every actual canonical worker occurrence, including identical
            // tuples; suppress only repeated await projections of those facts.
            if (awaited && !awaited_recorded && !state.command_contains(*outcome, *awaited))
                state.append(*awaited);
            if (!quiescent && (!awaited || !same_failure(*awaited, quiescent.error())) &&
                !state.command_contains(*outcome, quiescent.error()))
                state.append(quiescent.error());
        } else {
            if (awaited && !awaited_recorded) state.append(*awaited);
            if (!quiescent && (!awaited || !same_failure(*awaited, quiescent.error())))
                state.append(quiescent.error());
            if ((!awaited || !same_failure(*awaited, outcome.error())) &&
                (quiescent || !same_failure(quiescent.error(), outcome.error())))
                state.append(outcome.error());
        }
    };
    auto all_ready = generation->await_ready();
    if (!all_ready) {
        state.append(all_ready.error());
        generation->cancel_before_release();
        const PairwiseEvidenceFailure awaited{all_ready.error()};
        quiesce_cleanup(&awaited, true);
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    if (state.cancellation.requested()) {
        generation->cancel_before_release();
        quiesce_cleanup();
        return std::unexpected{FrozenObservationRunReason::cancelled};
    }
    auto capture = state.monitoring->begin_capture(state.dependencies.platform);
    if (!capture) {
        state.capture_failure(std::move(capture.error()));
        generation->cancel_before_release();
        quiesce_cleanup();
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    const auto abort = [&]() noexcept {
        auto result = state.monitoring->abort_capture(std::move(*capture));
        if (!result)
            state.capture_failure(std::move(result.error()));
    };
    auto start = state.clock(PairwiseTypedClockSite::controller_start);
    if (!start) {
        state.append(start.error());
        generation->cancel_before_release();
        quiesce_cleanup();
        abort();
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    if (state.cancellation.requested()) {
        generation->cancel_before_release();
        quiesce_cleanup();
        abort();
        return std::unexpected{FrozenObservationRunReason::cancelled};
    }
    generation->release();
    auto timed = generation->await_timed_completion();
    if (!timed) {
        const PairwiseEvidenceFailure awaited{timed.error()};
        quiesce_cleanup(&awaited);
        abort();
        return std::unexpected{timed.error().reason() == PairwiseFailureReason::runner_cancelled
            ? FrozenObservationRunReason::cancelled : FrozenObservationRunReason::operational_failure};
    }
    if (state.cancellation.requested()) {
        quiesce_cleanup();
        abort();
        return std::unexpected{FrozenObservationRunReason::cancelled};
    }
    auto end = state.clock(PairwiseTypedClockSite::controller_end);
    if (!end) {
        state.append(end.error());
        quiesce_cleanup();
        abort();
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    PairwiseMeasurementLeafFacade measurement;
    auto enclosing =
        measurement.measurement_interval(*start, *end, PairwiseVerificationPhase::before);
    if (!enclosing) {
        state.append(enclosing.error());
        quiesce_cleanup();
        abort();
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    auto quiescent = generation->await_quiescence();
    auto outcome = generation->collect_ordered_results();
    if (outcome) {
        state.command_failures(*outcome);
        if (!quiescent && !state.command_contains(*outcome, quiescent.error()))
            state.append(quiescent.error());
    } else {
        if (!quiescent) state.append(quiescent.error());
        if (quiescent || !same_failure(quiescent.error(), outcome.error()))
            state.append(outcome.error());
    }
    if (state.failures.size() != start_failures) {
        abort();
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    if (state.cancellation.requested()) {
        abort();
        return std::unexpected{FrozenObservationRunReason::cancelled};
    }
    auto& destination = state.destinations[ordinal];
    for (std::size_t index{}; index < state.owners.size(); ++index) {
        auto& slot = state.owners[index];
        if (!slot.start || !slot.end) {
            state.append(runtime_failure(PairwiseLifecyclePhase::active));
            abort();
            return std::unexpected{FrozenObservationRunReason::operational_failure};
        }
        auto interval = measurement.measurement_interval(*slot.start, *slot.end,
                                                         PairwiseVerificationPhase::before);
        if (!interval) {
            state.append(interval.error());
            abort();
            return std::unexpected{FrozenObservationRunReason::operational_failure};
        }
        if (bandwidth)
            destination.bandwidth.push_back(
                measurement::SequentialReadBandwidthWorkerObservation::create(
                    state.ids[index], *interval, *slot.work, *slot.bandwidth_verification));
        else
            destination.latency.push_back(
                measurement::DependentLoadLatencyWorkerObservation::create(
                    state.ids[index], *interval, *slot.schedule, *slot.latency_verification));
        const auto qualification = slot.owner->region().qualification_state();
        const auto placement = qualification == memory::PlacementQualificationState::skipped
                                   ? measurement::WorkerPlacementValidityState::not_applicable
                               : qualification == memory::PlacementQualificationState::qualified
                                   ? measurement::WorkerPlacementValidityState::qualified
                                   : measurement::WorkerPlacementValidityState::not_qualified;
        destination.placements.push_back(
            measurement::WorkerPlacementValidityEvidence::create(state.ids[index], placement));
        destination.migrations.push_back(measurement::WorkerMigrationValidityEvidence::create(
            state.ids[index], outcome->workers()[index].migrated
                                  ? measurement::WorkerMigrationValidityState::migrated
                                  : measurement::WorkerMigrationValidityState::not_migrated));
    }
    auto factual =
        [&]() -> std::expected<measurement::MeasurementObservation, PairwiseMeasurementFailure> {
        if (bandwidth) {
            auto workers =
                measurement.validate_bandwidth_workers(state.ids, std::move(destination.bandwidth));
            if (!workers)
                return std::unexpected{std::move(workers.error())};
            auto observation = measurement.bandwidth_observation(
                observation_id, experiment, block, state.implementation,
                *state.bandwidth_configuration,
                std::get<kernel::SequentialReadBandwidthPassCount>(state.selected),
                std::move(*workers), *enclosing);
            if (!observation)
                return std::unexpected{std::move(observation.error())};
            return measurement::MeasurementObservation{std::move(*observation)};
        }
        auto workers =
            measurement.validate_latency_workers(state.ids, std::move(destination.latency));
        if (!workers)
            return std::unexpected{std::move(workers.error())};
        auto observation = measurement.latency_observation(
            observation_id, experiment, block, state.implementation, *state.latency_configuration,
            std::move(*workers), *enclosing);
        if (!observation)
            return std::unexpected{std::move(observation.error())};
        return measurement::MeasurementObservation{std::move(*observation)};
    }();
    if (!factual) {
        state.append(factual.error());
        abort();
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    auto captured = state.monitoring->complete_capture(std::move(*capture), *start, *end,
                                                       state.dependencies.platform);
    if (!captured) {
        state.capture_failure(std::move(captured.error()));
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    auto capture_result = std::move(*captured).take_result();
    if (!capture_result) {
        state.append(runtime_failure(PairwiseLifecyclePhase::active));
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    auto validity = std::visit(
        [&](const auto& observed) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(observed)>,
                                       measurement::SequentialReadBandwidthObservation>)
                return measurement.bandwidth_validity(
                    observed, *state.validity, state.overhead, destination.placements,
                    destination.migrations, monitoring_evidence(*capture_result));
            else
                return measurement.latency_validity(observed, *state.validity, state.overhead,
                                                    destination.placements, destination.migrations,
                                                    monitoring_evidence(*capture_result));
        },
        *factual);
    if (!validity) {
        state.append(validity.error());
        return std::unexpected{FrozenObservationRunReason::operational_failure};
    }
    state.phase = FrozenObservationRunState::Phase::ready;
    return FrozenObservation{observation_id,
                             experiment,
                             block,
                             std::move(*factual),
                             std::move(*validity),
                             std::move(*capture_result),
                             std::move(destination.placements),
                             std::move(destination.migrations)};
}

auto FrozenObservationRun::finish() && noexcept
    -> std::expected<FrozenObservationRetirement, FrozenObservationRunReason> {
    if (!state_ || state_->phase == FrozenObservationRunState::Phase::retired)
        return std::unexpected{FrozenObservationRunReason::unavailable};
    state_->retire();
    return FrozenObservationRetirement{std::move(state_)};
}
} // namespace epycbench::runtime::detail
