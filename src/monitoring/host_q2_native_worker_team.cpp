#include "monitoring/host_q2_native_worker_team_test_access.hpp"

#include "monitoring/host_q2_controlled_event_runtime_bridge.hpp"

#include <limits>

namespace epycbench::monitoring::qualification::detail {
namespace {
constexpr auto cancelled_word = std::numeric_limits<std::uint64_t>::max();
auto team_error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.native_worker_team"},
            .message = "Q2 native worker team lifecycle failed",
            .operation = "monitoring_q2_native_worker_team",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
constexpr auto compact(MonitoringQ2CompactOperation operation, int native = 0) noexcept
    -> MonitoringQ2CompactDiagnostic {
    return {operation, native};
}
} // namespace

struct MonitoringQ2NativeWorkerTeam::State final {
    struct Worker final {
        State* team{};
        runtime::detail::WorkerLaunchSpec spec;
        std::optional<runtime::WorkerReadyEvidence> ready;
        std::optional<core::Error> preparation_failure;
        runtime::detail::TargetedWorkerOperation operation{};
        runtime::detail::TargetedWorkerReadinessOperation readiness{};
        runtime::detail::TargetedWorkerCallbackResult result{
            runtime::detail::TargetedWorkerDisposition::invalid_callback_result, 0};
        std::stop_source command_stop;
        std::atomic<std::uint64_t> command{}, generation{}, ready_generation{}, timed_generation{},
            terminal_generation{}, park_offer{}, parked_generation{};
        // The owner-lifetime idle bound is independent of a generation's operation
        // deadline.  Reusing the latter here lets a worker observe an already-expired
        // trial deadline after parking and silently retire before the next begin.
        MonitoringQ2WaitContextCapsule idle_wait;
        MonitoringQ2WaitContextCapsule command_wait;
        std::optional<MonitoringQ2NativeThreadOwner> owner;
        explicit Worker(State* value, runtime::detail::WorkerLaunchSpec item)
            : team{value}, spec{std::move(item)} {}
    };
    runtime::detail::WorkerTeamDependencies dependencies;
    std::vector<std::unique_ptr<Worker>> workers;
    std::optional<std::size_t> active_index;
    std::stop_source team_stop;
    bool retired{}, quarantined{};
    core::Error invalid_error{team_error("state")};

    static auto prepare(void* raw) noexcept -> std::expected<void, core::Error> {
        auto& worker = *static_cast<Worker*>(raw);
        auto ready = runtime::detail::PersistentWorkerTeamAccess::startup(
            worker.spec, worker.team->dependencies.platform);
        if (!ready)
            return std::unexpected{std::move(ready.error())};
        worker.ready.emplace(std::move(*ready));
        return {};
    }
    static void signal_timed(void* raw) noexcept {
        auto& worker = *static_cast<Worker*>(raw);
        worker.timed_generation.store(worker.generation.load(std::memory_order_acquire),
                                      std::memory_order_release);
        monitoring_q2_atomic_wake(worker.timed_generation);
    }
    static void run(void* raw, MonitoringQ2NativeThreadOwner::StopToken native_stop) noexcept {
        auto& worker = *static_cast<Worker*>(raw);
        for (;;) {
            while (worker.command.load(std::memory_order_acquire) == 0U &&
                   !native_stop.stop_requested()) {
                auto parked = monitoring_q2_bounded_atomic_wait(worker.command, 0U,
                                                                worker.idle_wait.load());
                if (!parked)
                    return;
            }
            if (native_stop.stop_requested() ||
                worker.command.load(std::memory_order_acquire) == cancelled_word)
                return;
            const auto generation = worker.generation.load(std::memory_order_acquire);
            if (worker.command.load(std::memory_order_acquire) != 1U || generation == 0U ||
                !worker.ready)
                return;
            runtime::WorkerExecutionContext context{
                worker.spec.worker_index, worker.spec.worker_id,     worker.spec.processing_unit,
                worker.spec.core,         worker.spec.requested_cpu, *worker.ready};
            auto observed = worker.readiness.observe(worker.readiness.context, context);
            if (!observed) {
                worker.preparation_failure.emplace(std::move(observed.error()));
                worker.result = {runtime::detail::TargetedWorkerDisposition::caller_failure, 0};
                worker.ready_generation.store(cancelled_word, std::memory_order_release);
            } else
                worker.ready_generation.store(generation, std::memory_order_release);
            monitoring_q2_atomic_wake(worker.ready_generation);
            while (worker.command.load(std::memory_order_acquire) == 1U &&
                   !native_stop.stop_requested()) {
                auto released = monitoring_q2_bounded_atomic_wait(worker.command, 1U,
                                                                  worker.command_wait.load());
                if (!released)
                    return;
            }
            const auto command = worker.command.load(std::memory_order_acquire);
            if (native_stop.stop_requested() || command == cancelled_word)
                return;
            if (command == 3U) {
                worker.result = {runtime::detail::TargetedWorkerDisposition::caller_failure, 0};
            } else if (command == 2U) {
                runtime::detail::TargetedWorkerExecutionControl control{
                    worker.team->team_stop.get_token(), worker.command_stop.get_token(), nullptr,
                    &worker, signal_timed};
                worker.result = worker.operation.run(worker.operation.context, context, control);
            } else
                return;
            worker.terminal_generation.store(generation, std::memory_order_release);
            monitoring_q2_atomic_wake(worker.terminal_generation);
            for (;;) {
                const auto offered_value = worker.park_offer.load(std::memory_order_acquire);
                // Retirement is not a successful park offer. Observe its owner stop and
                // sentinel before the bounded wait (which cannot accept this sentinel).
                if (native_stop.stop_requested() || offered_value == cancelled_word)
                    return;
                if (offered_value == generation)
                    break;
                auto offered = monitoring_q2_bounded_atomic_wait(worker.park_offer, offered_value,
                                                                 worker.command_wait.load());
                if (!offered)
                    return;
            }
            // A callback completing after retirement must not replace its stop word.
            auto expected_command = command;
            if (!worker.command.compare_exchange_strong(expected_command, 0U,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
                return;
            monitoring_q2_atomic_wake(worker.command);
            worker.parked_generation.store(generation, std::memory_order_release);
            monitoring_q2_atomic_wake(worker.parked_generation);
        }
    }
};

MonitoringQ2NativeWorkerTeam::MonitoringQ2NativeWorkerTeam(std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}
MonitoringQ2NativeWorkerTeam::MonitoringQ2NativeWorkerTeam(
    MonitoringQ2NativeWorkerTeam&&) noexcept = default;
MonitoringQ2NativeWorkerTeam::~MonitoringQ2NativeWorkerTeam() noexcept {
    if (state_ && !state_->retired)
        std::terminate();
}

auto MonitoringQ2NativeWorkerTeam::create(
    runtime::detail::MonitoringQ1AdmittedWorkerSnapshot&& snapshot,
    runtime::detail::WorkerTeamDependencies dependencies,
    MonitoringQ2DeadlineCancellationContext wait,
    const MonitoringQ2NativeThreadSystem* injected) noexcept
    -> MonitoringQ2NativeWorkerTeamCreateOutcome {
    auto fail = [](MonitoringQ2CompactDiagnostic value) {
        return MonitoringQ2NativeWorkerTeamCreateOutcome{std::nullopt,
                                                         CreateFailure{std::move(value)}};
    };
    std::unique_ptr<State> state;
    try {
        if (snapshot.records_.empty() ||
            snapshot.records_.size() > MonitoringQ2NativeThreadOwner::maximum_targets)
            return fail(compact(MonitoringQ2CompactOperation::worker_count));
        state = std::make_unique<State>();
        state->dependencies = std::move(dependencies);
        state->workers.reserve(snapshot.records_.size());
        for (auto& spec : snapshot.records_)
            state->workers.push_back(std::make_unique<State::Worker>(state.get(), std::move(spec)));
        for (std::size_t index{}; index < state->workers.size(); ++index) {
            auto& worker = *state->workers[index];
            worker.idle_wait.publish({wait.deadline, {}});
            auto created = MonitoringQ2NativeThreadOwner::create_parked(
                {MonitoringQ2NativeThreadRole::target, index}, &worker, State::run, wait, injected,
                State::prepare);
            if (!created) {
                auto native_failure = std::move(created).take_failure();
                CreateFailure failure{native_failure.primary};
                for (std::size_t i{}; i < native_failure.cleanup.size(); ++i)
                    failure.cleanup.append(native_failure.cleanup[i]);
                if (native_failure.cleanup.overflowed())
                    failure.cleanup.note_overflow();
                failure.authored_primary = std::move(native_failure.authored_primary);
                auto recovery = std::move(created).recovery_owner();
                if (recovery)
                    worker.owner.emplace(std::move(*recovery));
                state->quarantined = true;
                return MonitoringQ2NativeWorkerTeamCreateOutcome{
                    MonitoringQ2NativeWorkerTeam{std::move(state)}, std::move(failure)};
            }
            worker.owner.emplace(std::move(created).owner());
        }
        for (auto& worker : state->workers) {
            auto started = worker->owner->release_start();
            if (!started) {
                state->quarantined = true;
                return MonitoringQ2NativeWorkerTeamCreateOutcome{
                    MonitoringQ2NativeWorkerTeam{std::move(state)},
                    CreateFailure{started.error()}};
            }
        }
        return MonitoringQ2NativeWorkerTeamCreateOutcome{
            MonitoringQ2NativeWorkerTeam{std::move(state)}, std::nullopt};
    } catch (...) {
        if (state) {
            state->quarantined = true;
            return MonitoringQ2NativeWorkerTeamCreateOutcome{
                MonitoringQ2NativeWorkerTeam{std::move(state)},
                CreateFailure{compact(MonitoringQ2CompactOperation::construction)}};
        }
        return fail(compact(MonitoringQ2CompactOperation::construction));
    }
}

auto MonitoringQ2NativeWorkerTeam::readiness(std::size_t index) const noexcept
    -> std::expected<runtime::WorkerReadyEvidence, core::Error> {
    if (!state_ || index >= state_->workers.size() || !state_->workers[index]->ready)
        return std::unexpected{team_error("readiness")};
    return *state_->workers[index]->ready;
}

auto MonitoringQ2NativeWorkerTeam::generation_operations() noexcept
    -> Q2RuntimeGenerationOperations {
    return {
        state_.get(),
        [](void* raw, std::size_t index, runtime::detail::TargetedWorkerOperation operation,
           runtime::detail::TargetedWorkerReadinessOperation readiness,
           MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (team.retired || team.quarantined || team.active_index ||
                index >= team.workers.size())
                return std::unexpected{team_error("begin")};
            auto& worker = *team.workers[index];
            if (worker.command.load() != 0U)
                return std::unexpected{team_error("busy")};
            worker.operation = operation;
            worker.readiness = readiness;
            worker.preparation_failure.reset();
            worker.ready_generation.store(0U, std::memory_order_release);
            worker.command_stop = std::stop_source{};
            worker.command_wait.publish(wait);
            const auto generation = worker.generation.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            if (generation == 0U || generation == cancelled_word)
                return std::unexpected{team_error("generation")};
            team.active_index = index;
            worker.command.store(1U, std::memory_order_release);
            monitoring_q2_atomic_wake(worker.command);
            return {};
        },
        [](void* raw, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("generation_missing")};
            auto& item = team.workers[*team.active_index];
            const auto generation = item->generation.load();
            for (;;) {
                const auto observed = item->ready_generation.load();
                if (observed == generation)
                    break;
                if (observed == cancelled_word) {
                    if (item->preparation_failure)
                        return std::unexpected{std::move(*item->preparation_failure)};
                    return std::unexpected{team_error("readiness")};
                }
                auto result =
                    monitoring_q2_bounded_atomic_wait(item->ready_generation, observed, wait);
                if (!result) {
                    team.quarantined = true;
                    return result;
                }
            }
            return {};
        },
        [](void* raw) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("release")};
            auto& item = team.workers[*team.active_index];
            if (item->command.load() != 1U)
                return std::unexpected{team_error("release")};
            item->command.store(2U);
            monitoring_q2_atomic_wake(item->command);
            return {};
        },
        [](void* raw, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("timed_missing")};
            auto& item = team.workers[*team.active_index];
            const auto generation = item->generation.load();
            for (;;) {
                const auto observed = item->timed_generation.load();
                if (observed == generation)
                    break;
                auto result =
                    monitoring_q2_bounded_atomic_wait(item->timed_generation, observed, wait);
                if (!result) {
                    item->command_stop.request_stop();
                    team.quarantined = true;
                    return result;
                }
            }
            return {};
        },
        [](void* raw, MonitoringQ2DeadlineCancellationContext wait) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("terminal_missing")};
            auto& item = team.workers[*team.active_index];
            const auto generation = item->generation.load();
            for (;;) {
                const auto observed = item->terminal_generation.load();
                if (observed == generation)
                    break;
                auto result =
                    monitoring_q2_bounded_atomic_wait(item->terminal_generation, observed, wait);
                if (!result) {
                    item->command_stop.request_stop();
                    team.quarantined = true;
                    return result;
                }
            }
            item->command_wait.publish({wait.deadline, {}});
            item->park_offer.store(generation, std::memory_order_release);
            monitoring_q2_atomic_wake(item->park_offer);
            for (;;) {
                const auto observed = item->command.load(std::memory_order_acquire);
                if (observed == 0U)
                    break;
                auto result = monitoring_q2_bounded_atomic_wait(item->command, observed, wait);
                if (!result) {
                    item->command_stop.request_stop();
                    team.quarantined = true;
                    return result;
                }
            }
            for (;;) {
                const auto observed = item->parked_generation.load(std::memory_order_acquire);
                if (observed == generation)
                    break;
                auto result =
                    monitoring_q2_bounded_atomic_wait(item->parked_generation, observed, wait);
                if (!result) {
                    team.quarantined = true;
                    return result;
                }
            }
            return {};
        },
        [](void* raw) noexcept -> std::expected<bool, core::Error> {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("result_missing")};
            auto& item = team.workers[*team.active_index];
            if (item->terminal_generation.load() != item->generation.load())
                return std::unexpected{team_error("result_missing")};
            return item->result.disposition ==
                   runtime::detail::TargetedWorkerDisposition::succeeded;
        },
        [](void* raw) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("cancel_ready")};
            auto& item = team.workers[*team.active_index];
            if (item->command.load() != 1U)
                return std::unexpected{team_error("cancel_ready")};
            item->command.store(3U);
            monitoring_q2_atomic_wake(item->command);
            return {};
        },
        [](void* raw) noexcept -> Q2LifecycleResult {
            auto& team = *static_cast<State*>(raw);
            if (!team.active_index)
                return std::unexpected{team_error("reset")};
            auto& item = team.workers[*team.active_index];
            if (item->terminal_generation.load() != item->generation.load() ||
                item->command.load() != 0U)
                return std::unexpected{team_error("reset")};
            item->operation = {};
            item->readiness = {};
            team.active_index.reset();
            return {};
        }};
}

auto MonitoringQ2NativeWorkerTeam::retire(MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, CreateFailure> {
    if (!state_ || state_->retired)
        std::terminate();
    CreateFailure failure{compact(MonitoringQ2CompactOperation::retire)};
    bool failed{};
    state_->team_stop.request_stop();
    for (auto& worker : state_->workers) {
        if (!worker->owner || !worker->owner->active())
            continue;
        worker->owner->request_stop();
        worker->command.store(cancelled_word, std::memory_order_release);
        worker->park_offer.store(cancelled_word, std::memory_order_release);
        monitoring_q2_atomic_wake(worker->command);
        monitoring_q2_atomic_wake(worker->park_offer);
    }
    for (auto& worker : state_->workers) {
        if (!worker->owner || !worker->owner->active())
            continue;
        auto joined = worker->owner->timed_join(wait);
        if (!joined) {
            if (!failed) {
                failure.primary = joined.error();
                failed = true;
            } else
                failure.cleanup.append(joined.error());
        }
        auto cleanup = worker->owner->take_cleanup_diagnostics();
        for (std::size_t index{}; index < cleanup.size(); ++index)
            failure.cleanup.append(cleanup[index]);
        if (cleanup.overflowed()) failure.cleanup.note_overflow();
    }
    bool active{};
    for (const auto& worker : state_->workers)
        active = active || (worker->owner && worker->owner->active());
    if (active) {
        state_->quarantined = true;
        return std::unexpected{std::move(failure)};
    }
    state_->retired = true;
    state_->quarantined = false;
    if (failed)
        return std::unexpected{std::move(failure)};
    return {};
}
auto MonitoringQ2NativeWorkerTeam::active() const noexcept -> bool {
    return state_ && !state_->retired;
}
auto MonitoringQ2NativeWorkerTeam::cancellation_requested(std::size_t index) const noexcept
    -> bool {
    if (!state_ || state_->retired || state_->quarantined ||
        index >= state_->workers.size() || !state_->active_index ||
        *state_->active_index != index)
        return true;
    return state_->team_stop.stop_requested() ||
           state_->workers[index]->command_stop.stop_requested();
}

auto MonitoringQ2NativeWorkerTeamTestAccess::terminal_without_park_offer(
    const MonitoringQ2NativeWorkerTeam& team, std::size_t index) noexcept -> bool {
    if (!team.state_ || index >= team.state_->workers.size())
        std::terminate();
    const auto& worker = *team.state_->workers[index];
    const auto generation = worker.generation.load(std::memory_order_acquire);
    return generation != 0U &&
           worker.terminal_generation.load(std::memory_order_acquire) == generation &&
           worker.park_offer.load(std::memory_order_acquire) != generation;
}
void MonitoringQ2NativeWorkerTeamTestAccess::request_command_stop(
    MonitoringQ2NativeWorkerTeam& team, std::size_t index) noexcept {
    if (!team.state_ || !team.state_->active_index || *team.state_->active_index != index ||
        index >= team.state_->workers.size())
        std::terminate();
    static_cast<void>(team.state_->workers[index]->command_stop.request_stop());
}
void MonitoringQ2NativeWorkerTeamTestAccess::request_team_stop(
    MonitoringQ2NativeWorkerTeam& team) noexcept {
    if (!team.state_ || team.state_->retired)
        std::terminate();
    static_cast<void>(team.state_->team_stop.request_stop());
}

} // namespace epycbench::monitoring::qualification::detail
