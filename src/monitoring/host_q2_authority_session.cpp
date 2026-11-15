#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_context_switch_source.hpp"
#include "monitoring/host_q2_controlled_event_runtime_bridge.hpp"
#include <limits>
#include <unistd.h>
#include <utility>

namespace epycbench::monitoring::qualification::detail {
namespace {
auto error(std::string classification) -> core::Error {
    return {.category = core::ErrorCategory::monitoring,
            .code = core::StableErrorCode{"monitoring.q2.authority.invalid"},
            .message = "Q2 authority rejected",
            .operation = "authenticate_monitoring_q2_authority",
            .os_error = std::nullopt,
            .context = {{"classification", std::move(classification)}},
            .severity = core::ErrorSeverity::error,
            .continuation = core::ContinuationPolicy::abort_command};
}
auto wrapped(std::string classification) -> MonitoringQ1GitCommandFailure {
    return {error(std::move(classification)), {}};
}
} // namespace
struct MonitoringQ2PageSizeRecheck::State final {
    std::vector<core::WorkerId> workers;
    std::uint64_t expected{};
    Operations operations{};
    std::size_t issued{}, consumed{};
    bool outstanding{}, failed{}, active{true};
};
MonitoringQ2PageSizeRecheck::MonitoringQ2PageSizeRecheck(
    MonitoringQ2PageSizeRecheck&& other) noexcept
    : expected_{other.expected_}, operations_{other.operations_}, state_{other.state_},
      slot_{other.slot_}, consumed_{std::exchange(other.consumed_, true)} {
    other.state_ = nullptr;
}
MonitoringQ2PageSizeRecheck::~MonitoringQ2PageSizeRecheck() noexcept {
    if (state_ && !consumed_)
        std::terminate();
}
auto MonitoringQ2PageSizeRecheck::consume() noexcept -> std::expected<std::uint64_t, core::Error> {
    if (consumed_)
        std::terminate();
    consumed_ = true;
    if (state_) {
        if (!state_->active || !state_->outstanding || slot_ + 1U != state_->issued ||
            slot_ != state_->consumed)
            std::terminate();
        state_->outstanding = false;
        ++state_->consumed;
    }
    if (!operations_.page_size) {
        if (state_)
            state_->failed = true;
        return std::unexpected{error("page_size_operation")};
    }
    const long value = operations_.page_size(operations_.context);
    if (value <= 0 || static_cast<std::uint64_t>(value) != expected_) {
        if (state_)
            state_->failed = true;
        return std::unexpected{error("page_size_changed")};
    }
    return expected_;
}
MonitoringQ2PageSizeIssuer::MonitoringQ2PageSizeIssuer(
    std::span<const MonitoringQ2WorkerBinding> workers, std::uint64_t expected,
    MonitoringQ2PageSizeRecheck::Operations operations)
    : state_{std::make_unique<MonitoringQ2PageSizeRecheck::State>()} {
    state_->workers.reserve(workers.size());
    for (const auto& worker : workers)
        state_->workers.push_back(worker.worker);
    state_->expected = expected;
    state_->operations = operations;
}
MonitoringQ2PageSizeIssuer::MonitoringQ2PageSizeIssuer(MonitoringQ2PageSizeIssuer&& other) noexcept
    : state_{std::move(other.state_)} {}
MonitoringQ2PageSizeIssuer::~MonitoringQ2PageSizeIssuer() noexcept {
    if (state_ && state_->active)
        std::terminate();
}
auto MonitoringQ2PageSizeIssuer::issue(core::WorkerId worker) noexcept
    -> MonitoringQ2PageSizeRecheck {
    if (!state_ || !state_->active || state_->failed || state_->outstanding ||
        state_->issued >= state_->workers.size() || state_->workers[state_->issued] != worker)
        std::terminate();
    const auto slot = state_->issued++;
    state_->outstanding = true;
    return MonitoringQ2PageSizeRecheck{state_->expected, state_->operations, state_.get(), slot};
}
void MonitoringQ2PageSizeIssuer::block() noexcept {
    if (!state_ || !state_->active || state_->outstanding)
        std::terminate();
    state_->failed = true;
}
auto MonitoringQ2PageSizeIssuer::slots() const noexcept -> std::size_t {
    if (!state_)
        std::terminate();
    return state_->workers.size();
}
auto MonitoringQ2PageSizeIssuer::retire(bool successful) noexcept
    -> std::expected<void, core::Error> {
    if (!state_ || !state_->active || state_->outstanding)
        std::terminate();
    state_->active = false;
    if (successful && (state_->failed || state_->issued != state_->workers.size() ||
                       state_->consumed != state_->workers.size()))
        return std::unexpected{error("page_size_incomplete")};
    return {};
}
MonitoringQ2SourcePreparationWorkerHandoff::MonitoringQ2SourcePreparationWorkerHandoff(
    MonitoringQ2WorkerSession* owner, std::size_t index, Q2TargetSnapshot target,
    MonitoringQ2PageSizeRecheck&& page, Q2RuntimeGenerationOperations operations) noexcept
    : owner_{owner}, index_{index}, target_{std::move(target)}, page_recheck_{std::move(page)},
      runtime_operations_{operations} {}
MonitoringQ2SourcePreparationWorkerHandoff::MonitoringQ2SourcePreparationWorkerHandoff(
    MonitoringQ2SourcePreparationWorkerHandoff&& other) noexcept
    : owner_{other.owner_}, index_{other.index_}, target_{std::move(other.target_)},
      page_recheck_{std::move(other.page_recheck_)}, runtime_operations_{other.runtime_operations_},
      consumed_{std::exchange(other.consumed_, true)} {}
MonitoringQ2SourcePreparationWorkerHandoff::~MonitoringQ2SourcePreparationWorkerHandoff() noexcept {
    if (!consumed_)
        std::terminate();
}
auto MonitoringQ2SourcePreparationWorkerHandoff::take_page_recheck() noexcept
    -> MonitoringQ2PageSizeRecheck {
    if (consumed_ || !page_recheck_)
        std::terminate();
    auto result = std::move(*page_recheck_);
    page_recheck_.reset();
    return result;
}
void MonitoringQ2SourcePreparationWorkerHandoff::cancel_before_prepare() noexcept {
    if (consumed_ || !owner_ || !page_recheck_)
        std::terminate();
    static_cast<void>(page_recheck_->consume());
    page_recheck_.reset();
    fail();
}
void MonitoringQ2SourcePreparationWorkerHandoff::fail() noexcept {
    if (consumed_ || !owner_ || page_recheck_)
        std::terminate();
    owner_->release(index_, false);
    consumed_ = true;
}
MonitoringQ2WorkerSession::MonitoringQ2WorkerSession(
    runtime::detail::MonitoringQ1AdmittedWorkerSnapshot snapshot, MonitoringQ2PageSizeIssuer issuer)
    : snapshot_{std::move(snapshot)}, issuer_{std::move(issuer)},
      activation_error_{error("activation")}, incomplete_error_{error("worker_session_incomplete")},
      association_error_{error("worker_final_association")},
      overflow_error_{error("cleanup_overflow")} {
    finalization_ledger_.reserve(1024U);
    injected_finalization_cleanup_.reserve(67U);
    deferred_source_states_.reserve(issuer_.slots());
}
MonitoringQ2WorkerSession::MonitoringQ2WorkerSession(MonitoringQ2WorkerSession&& other) noexcept
    : snapshot_{std::move(other.snapshot_)}, runtime_{std::move(other.runtime_)},
      native_runtime_{std::move(other.native_runtime_)}, issuer_{std::move(other.issuer_)},
      next_worker_{other.next_worker_}, borrowed_{other.borrowed_}, blocked_{other.blocked_},
      finalized_{other.finalized_}, injected_runtime_{other.injected_runtime_},
      activation_fault_{other.activation_fault_},
      injected_finalization_primary_{std::move(other.injected_finalization_primary_)},
      injected_finalization_cleanup_{std::move(other.injected_finalization_cleanup_)},
      deferred_source_states_{std::move(other.deferred_source_states_)},
      post_inactive_context_{other.post_inactive_context_},
      post_inactive_hook_{other.post_inactive_hook_},
      activation_error_{std::move(other.activation_error_)},
      incomplete_error_{std::move(other.incomplete_error_)},
      association_error_{std::move(other.association_error_)},
      overflow_error_{std::move(other.overflow_error_)},
      finalization_ledger_{std::move(other.finalization_ledger_)},
      finalization_overflowed_{other.finalization_overflowed_},
      finalization_started_{other.finalization_started_},
      finalization_successful_{other.finalization_successful_},
      finalization_primary_{std::move(other.finalization_primary_)},
      native_diagnostics_{std::move(other.native_diagnostics_)},
      controlled_diagnostics_{other.controlled_diagnostics_} {
    other.snapshot_.reset();
    other.runtime_.reset();
    other.native_runtime_.reset();
    other.borrowed_ = false;
    other.finalized_ = true;
}
MonitoringQ2WorkerSession::~MonitoringQ2WorkerSession() noexcept {
    if (finalization_started_ && !finalized_)
        std::terminate();
}
auto MonitoringQ2WorkerSession::take_snapshot() noexcept
    -> runtime::detail::MonitoringQ1AdmittedWorkerSnapshot {
    if (!snapshot_)
        std::terminate();
    auto result = std::move(*snapshot_);
    snapshot_.reset();
    return result;
}
auto MonitoringQ2WorkerSession::activate(MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> std::expected<void, core::Error> {
    return activate_native_with(wait, runtime::detail::production_worker_team_dependencies());
}
auto MonitoringQ2WorkerSession::activate_native_with(
    MonitoringQ2DeadlineCancellationContext wait,
    runtime::detail::WorkerTeamDependencies dependencies,
    const MonitoringQ2NativeThreadSystem* system) noexcept -> std::expected<void, core::Error> {
    if (finalized_ || runtime_ || native_runtime_ || !snapshot_)
        std::terminate();
    auto value = MonitoringQ2NativeWorkerTeam::create(take_snapshot(), std::move(dependencies),
                                                      wait, system);
    if (!value) {
        blocked_ = true;
        issuer_.block();
        auto failure = std::move(value).take_failure();
        native_diagnostics_.emplace(std::move(failure));
        auto recovery = std::move(value).recovery_team();
        if (recovery)
            native_runtime_.emplace(std::move(*recovery));
        return std::unexpected{activation_error_};
    }
    native_runtime_.emplace(std::move(value).team());
    return {};
}
auto MonitoringQ2WorkerSession::activate_with(
    runtime::RunnerCancellation& cancellation,
    runtime::detail::WorkerTeamDependencies dependencies) noexcept
    -> std::expected<void, core::Error> {
    if (finalized_ || runtime_ || native_runtime_ || !snapshot_)
        std::terminate();
    try {
        if (activation_fault_ == 1U)
            throw std::bad_alloc{};
        if (activation_fault_ == 2U)
            throw std::runtime_error{"injected activation failure"};
        auto value = runtime::detail::make_monitoring_q1_worker_session(
            take_snapshot(), cancellation, std::move(dependencies));
        if (!value) {
            blocked_ = true;
            issuer_.block();
            return std::unexpected{std::move(value.error())};
        }
        runtime_.emplace(std::move(*value));
        return {};
    } catch (...) {
        blocked_ = true;
        issuer_.block();
        return std::unexpected{std::move(activation_error_)};
    }
}
void MonitoringQ2WorkerSession::activate_injected() noexcept {
    if (finalized_ || runtime_ || !snapshot_ || injected_runtime_)
        std::terminate();
    snapshot_.reset();
    injected_runtime_ = true;
}
auto MonitoringQ2WorkerSession::borrow(std::size_t index) noexcept
    -> runtime::detail::MonitoringQ1WorkerSession* {
    if (finalized_ || finalization_started_ || blocked_ || borrowed_ ||
        (!runtime_ && !native_runtime_ && !injected_runtime_) || index != next_worker_)
        std::terminate();
    borrowed_ = true;
    return runtime_ ? &*runtime_ : nullptr;
}
auto MonitoringQ2WorkerSession::begin_source_preparation(
    std::size_t index, const MonitoringQ2WorkerBinding& binding) noexcept
    -> std::expected<MonitoringQ2SourcePreparationWorkerHandoff, core::Error> {
    static_cast<void>(borrow(index));
    try {
        if (!native_runtime_) {
            release(index, false);
            return std::unexpected{std::move(association_error_)};
        }
        auto ready = native_runtime_->readiness(index);
        if (!ready) {
            release(index, false);
            return std::unexpected{std::move(ready.error())};
        }
        Q2TargetSnapshot target{
            ready->worker_id,   ready->requested_cpu, binding.core, binding.processing_unit,
            ready->stable_core, ready->stable_pu,     ready->tid,   true};
        if (target.worker != binding.worker || target.cpu != binding.linux_cpu ||
            target.stable_core != binding.stable_core ||
            target.stable_pu != binding.stable_processing_unit) {
            release(index, false);
            return std::unexpected{std::move(association_error_)};
        }
        auto page = issuer_.issue(binding.worker);
        auto operations = native_runtime_->generation_operations();
        return MonitoringQ2SourcePreparationWorkerHandoff{this, index, std::move(target),
                                                          std::move(page), operations};
    } catch (...) {
        release(index, false);
        return std::unexpected{std::move(association_error_)};
    }
}
void MonitoringQ2WorkerSession::release(std::size_t index, bool completed) noexcept {
    if (finalized_ || !borrowed_ || index != next_worker_)
        std::terminate();
    borrowed_ = false;
    if (completed) {
        ++next_worker_;
        return;
    }
    blocked_ = true;
    issuer_.block();
}
auto MonitoringQ2WorkerSession::complete_worker(
    MonitoringQ2QuiescedWorkerBorrow&& handoff, MonitoringQ2ContextSourceFinalization&& source,
    MonitoringQ2DeadlineCancellationContext wait) noexcept -> std::expected<void, core::Error> {
    if (finalized_ || blocked_ || !borrowed_ || handoff.consumed_ || source.consumed_ ||
        handoff.owner_ != this || handoff.index_ != next_worker_)
        std::terminate();
    handoff.consumed_ = true;
    deferred_source_states_.push_back(std::move(source.deferred_state_));
    source.consumed_ = true;
    auto live = handoff.operations_.revalidate(handoff.operations_.context, handoff.index_, wait);
    const auto* now = handoff.operations_.snapshot(handoff.operations_.context, handoff.index_);
    const auto& source_binding = source.binding_;
    if (!live || !now || source_binding.worker != handoff.expected_.worker ||
        source_binding.linux_cpu != handoff.expected_.cpu ||
        source_binding.core != handoff.expected_.core ||
        source_binding.processing_unit != handoff.expected_.pu ||
        source_binding.stable_core != handoff.expected_.stable_core ||
        source_binding.stable_processing_unit != handoff.expected_.stable_pu ||
        source.tid_ != handoff.expected_.tid || now->worker != handoff.expected_.worker ||
        now->cpu != handoff.expected_.cpu || now->core != handoff.expected_.core ||
        now->pu != handoff.expected_.pu || now->tid != handoff.expected_.tid ||
        now->stable_core != handoff.expected_.stable_core ||
        now->stable_pu != handoff.expected_.stable_pu || !now->quiescent) {
        release(handoff.index_, false);
        return live ? std::unexpected{std::move(association_error_)}
                    : std::unexpected{std::move(live.error())};
    }
    release(handoff.index_, true);
    return {};
}
void MonitoringQ2WorkerSession::complete_worker_failure(
    MonitoringQ2QuiescedWorkerBorrow&& handoff, MonitoringQ2ContextSourceFailure&& source,
    MonitoringQ2DeadlineCancellationContext wait) noexcept {
    if (finalized_ || blocked_ || !borrowed_ || handoff.consumed_ || source.consumed_ ||
        handoff.owner_ != this || handoff.index_ != next_worker_ ||
        source.binding_.worker != handoff.expected_.worker ||
        source.binding_.linux_cpu != handoff.expected_.cpu ||
        source.binding_.core != handoff.expected_.core ||
        source.binding_.processing_unit != handoff.expected_.pu ||
        source.binding_.stable_core != handoff.expected_.stable_core ||
        source.binding_.stable_processing_unit != handoff.expected_.stable_pu ||
        source.tid_ != handoff.expected_.tid)
        std::terminate();
    handoff.consumed_ = true;
    deferred_source_states_.push_back(std::move(source.deferred_state_));
    source.consumed_ = true;
    injected_finalization_primary_.emplace(std::move(source.primary_));
    for (std::size_t i{}; i < source.cleanup_count_; ++i)
        injected_finalization_cleanup_.push_back(std::move(*source.cleanup_[i]));
    static_cast<void>(wait);
    const auto* now = handoff.operations_.snapshot(handoff.operations_.context, handoff.index_);
    if (!now || now->worker != handoff.expected_.worker || now->cpu != handoff.expected_.cpu ||
        now->core != handoff.expected_.core || now->pu != handoff.expected_.pu ||
        now->stable_core != handoff.expected_.stable_core ||
        now->stable_pu != handoff.expected_.stable_pu || now->tid != handoff.expected_.tid ||
        !now->quiescent)
        injected_finalization_cleanup_.push_back(std::move(association_error_));
    release(handoff.index_, false);
}
void MonitoringQ2WorkerSession::complete_worker_controlled_failure(
    MonitoringQ2RecoveredWorkerFailure&& recovered,
    MonitoringQ2DeadlineCancellationContext wait) noexcept {
    auto& handoff = recovered.borrow;
    if (finalized_ || blocked_ || !borrowed_ || handoff.consumed_ || handoff.owner_ != this ||
        handoff.index_ != next_worker_)
        std::terminate();
    handoff.consumed_ = true;
    if (recovered.failure.has_primary)
        injected_finalization_primary_.emplace(std::move(recovered.failure.primary));
    for (std::size_t index{}; index < recovered.failure.cleanup_count; ++index)
        injected_finalization_cleanup_.push_back(std::move(*recovered.failure.cleanup[index]));
    for (std::size_t index{}; index < recovered.failure.compact_cleanup.size(); ++index)
        controlled_diagnostics_.append(recovered.failure.compact_cleanup[index]);
    if (recovered.failure.compact_cleanup.overflowed())
        controlled_diagnostics_.note_overflow();
    static_cast<void>(wait);
    const auto* now = handoff.operations_.snapshot(handoff.operations_.context, handoff.index_);
    if (!now || now->worker != handoff.expected_.worker || now->cpu != handoff.expected_.cpu ||
        now->core != handoff.expected_.core || now->pu != handoff.expected_.pu ||
        now->stable_core != handoff.expected_.stable_core ||
        now->stable_pu != handoff.expected_.stable_pu || now->tid != handoff.expected_.tid ||
        !now->quiescent)
        injected_finalization_cleanup_.push_back(std::move(association_error_));
    release(handoff.index_, false);
}
void MonitoringQ2WorkerSession::complete_worker_combined_failure(
    MonitoringQ2RecoveredWorkerFailure&& recovered, MonitoringQ2ContextSourceFailure&& source,
    MonitoringQ2DeadlineCancellationContext wait) noexcept {
    if (source.consumed_)
        std::terminate();
    const auto retain = [&](core::Error value) {
        if (recovered.failure.cleanup_count < recovered.failure.cleanup.size())
            recovered.failure.cleanup[recovered.failure.cleanup_count++].emplace(std::move(value));
        else
            recovered.failure.cleanup_overflowed = true;
    };
    retain(std::move(source.primary_));
    for (std::size_t i{}; i < source.cleanup_count_; ++i)
        retain(std::move(*source.cleanup_[i]));
    source.consumed_ = true;
    deferred_source_states_.push_back(std::move(source.deferred_state_));
    complete_worker_controlled_failure(std::move(recovered), wait);
}
void MonitoringQ2WorkerSession::complete_worker_combined_failure(
    MonitoringQ2RecoveredWorkerFailure&& recovered, MonitoringQ2ContextSourceFinalization&& source,
    MonitoringQ2DeadlineCancellationContext wait) noexcept {
    if (source.consumed_)
        std::terminate();
    source.consumed_ = true;
    deferred_source_states_.push_back(std::move(source.deferred_state_));
    complete_worker_controlled_failure(std::move(recovered), wait);
}
auto MonitoringQ2WorkerSession::finalize(bool successful,
                                         MonitoringQ2DeadlineCancellationContext wait) noexcept
    -> Finalization {
    if (finalized_ || borrowed_ ||
        (finalization_started_ && successful != finalization_successful_))
        std::terminate();
    finalization_started_ = true;
    finalization_successful_ = successful;
    if (injected_retirement_retryable_) {
        if (!native_diagnostics_)
            native_diagnostics_.emplace(
                MonitoringQ2CompactDiagnostic{MonitoringQ2CompactOperation::join, ETIMEDOUT});
        return {std::move(finalization_primary_), std::move(finalization_ledger_), true};
    }
    bool compact_overflow_projected = false;
    const auto append = [&](core::Error value) {
        constexpr std::size_t limit = 64U;
        if (finalization_overflowed_)
            return;
        if (finalization_ledger_.size() + 1U < limit) {
            finalization_ledger_.push_back(std::move(value));
            return;
        }
        if (finalization_ledger_.size() < limit) {
            finalization_ledger_.push_back(std::move(overflow_error_));
            finalization_overflowed_ = true;
        }
    };
    if (runtime_) {
        auto runtime_result =
            runtime::detail::finalize_monitoring_q1_worker_session(std::move(*runtime_));
        runtime_.reset();
        auto runtime_primary = std::move(runtime_result).take_primary();
        if (runtime_primary) {
            if (!finalization_primary_)
                finalization_primary_ = std::move(runtime_primary);
            else
                append(std::move(*runtime_primary));
        }
        for (auto& item : std::move(runtime_result).take_secondary())
            append(std::move(item));
    }
    if (native_runtime_) {
        auto native = native_runtime_->retire(wait);
        if (!native) {
            auto next = std::move(native.error());
            if (!native_diagnostics_)
                native_diagnostics_.emplace(std::move(next));
            else {
                native_diagnostics_->cleanup.append(next.primary);
                for (std::size_t index{}; index < next.cleanup.size(); ++index)
                    native_diagnostics_->cleanup.append(next.cleanup[index]);
                if (next.cleanup.overflowed())
                    native_diagnostics_->cleanup.note_overflow();
                if (next.authored_primary && !native_diagnostics_->authored_primary)
                    native_diagnostics_->authored_primary = std::move(next.authored_primary);
            }
            if (native_runtime_->active())
                return {std::move(finalization_primary_), std::move(finalization_ledger_), true};
            native_runtime_.reset();
        } else
            native_runtime_.reset();
    }
    const bool complete = successful && !blocked_ && next_worker_ == issuer_.slots();
    snapshot_.reset();
    auto page = issuer_.retire(complete);
    if (post_inactive_hook_)
        post_inactive_hook_(post_inactive_context_);
    deferred_source_states_.clear();
    if (injected_finalization_primary_) {
        if (!finalization_primary_)
            finalization_primary_ = std::move(injected_finalization_primary_);
        else
            append(std::move(*injected_finalization_primary_));
        injected_finalization_primary_.reset();
    }
    for (auto& item : injected_finalization_cleanup_)
        append(std::move(item));
    injected_finalization_cleanup_.clear();
    if (native_diagnostics_) {
        if (native_diagnostics_->authored_primary) {
            if (!finalization_primary_)
                finalization_primary_ = std::move(native_diagnostics_->authored_primary);
            else
                append(std::move(*native_diagnostics_->authored_primary));
        } else if (!finalization_primary_)
            finalization_primary_.emplace(monitoring_q2_materialize(native_diagnostics_->primary));
        else
            append(monitoring_q2_materialize(native_diagnostics_->primary));
        for (std::size_t i{}; i < native_diagnostics_->cleanup.size(); ++i)
            append(monitoring_q2_materialize(native_diagnostics_->cleanup[i]));
        if (native_diagnostics_->cleanup.overflowed()) {
            append(monitoring_q2_materialize({MonitoringQ2CompactOperation::overflow, 0}));
            compact_overflow_projected = true;
        }
        native_diagnostics_.reset();
    }
    for (std::size_t index{}; index < controlled_diagnostics_.size(); ++index)
        append(monitoring_q2_materialize(controlled_diagnostics_[index]));
    if (controlled_diagnostics_.overflowed() && !compact_overflow_projected)
        append(monitoring_q2_materialize({MonitoringQ2CompactOperation::overflow, 0}));
    if (successful && !complete) {
        if (!finalization_primary_)
            finalization_primary_.emplace(std::move(incomplete_error_));
        else
            append(std::move(incomplete_error_));
    }
    if (!page) {
        if (!finalization_primary_)
            finalization_primary_.emplace(std::move(page.error()));
        else
            append(std::move(page.error()));
    }
    finalized_ = true;
    return {std::move(finalization_primary_), std::move(finalization_ledger_), false};
}
auto MonitoringQ2WorkerSession::take_native_containment_diagnostics() noexcept
    -> MonitoringQ2NativeWorkerTeam::CreateFailure {
    if (!native_diagnostics_)
        std::terminate();
    auto diagnostics = std::move(*native_diagnostics_);
    native_diagnostics_.reset();
    return diagnostics;
}
MonitoringQ2CollectorPlan::MonitoringQ2CollectorPlan(MonitoringQ2CollectorPlan&& other) noexcept
    : q1_{std::move(other.q1_)}, plan_{std::move(other.plan_)}, workers_{std::move(other.workers_)},
      page_size_{other.page_size_}, worker_session_{std::move(other.worker_session_)},
      running_build_identity_{other.running_build_identity_},
      running_build_attestation_{std::move(other.running_build_attestation_)},
      provenance_{std::move(other.provenance_)} {
    other.worker_session_.reset();
}
MonitoringQ2AuthoritySession::MonitoringQ2AuthoritySession(
    std::unique_ptr<MonitoringQ2Q1Authority> q1, MonitoringQ2PlanPublicInputAuthority input,
    MonitoringQ2PlanValue value, MonitoringQ2CompleteQ1PredecessorAuthority predecessor,
    MonitoringRunningBuildAuthority running_build, std::unique_ptr<HistoricalContext> historical,
    MonitoringQ2PageSizeRecheck::Operations page_operations,
    MonitoringQ1BoundedRetirement cleanup_ledger) noexcept
    : q1_{std::move(q1)}, input_{std::move(input)}, value_{std::move(value)},
      predecessor_{std::move(predecessor)}, running_build_{std::move(running_build)},
      historical_{std::move(historical)}, page_operations_{page_operations},
      cleanup_ledger_{std::move(cleanup_ledger)} {}
MonitoringQ2AuthoritySession::MonitoringQ2AuthoritySession(
    MonitoringQ2AuthoritySession&& other) noexcept
    : q1_{std::move(other.q1_)}, input_{std::move(other.input_)}, value_{std::move(other.value_)},
      predecessor_{std::move(other.predecessor_)}, running_build_{std::move(other.running_build_)},
      historical_{std::move(other.historical_)}, consumed_{std::exchange(other.consumed_, true)},
      page_operations_{other.page_operations_}, cleanup_ledger_{std::move(other.cleanup_ledger_)} {}
auto MonitoringQ2AuthoritySession::validated_build_attestation() const noexcept -> std::string_view {
    if (consumed_) std::terminate();
    return running_build_.canonical();
}
auto MonitoringQ2AuthoritySession::authenticated_repository_commit() const noexcept -> std::string_view {
    if (consumed_) std::terminate();
    return input_.captured_head();
}
auto MonitoringQ2AuthoritySession::retire_before_binding() noexcept -> PrebindRetirement {
    if (consumed_) std::terminate();
    consumed_ = true;
    PrebindRetirement result;
    const auto retain=[&](core::Error error) noexcept {
        if(result.count<result.errors.size()) result.errors[result.count++].emplace(std::move(error));
        else result.overflowed=true;
    };
    for(std::size_t index{};index<cleanup_ledger_.count;++index)
        retain(std::move(*cleanup_ledger_.failures[index]));
    result.overflowed=result.overflowed||cleanup_ledger_.overflowed;
    cleanup_ledger_={};
    auto build = running_build_.retire();
    if (!build) retain(std::move(build.error()));
    auto predecessor = predecessor_.retire();
    if (!predecessor) {
        retain(std::move(predecessor.error().primary));
        for (std::size_t i{}; i < predecessor.error().cleanup_count; ++i)
            retain(std::move(*predecessor.error().cleanup[i]));
    }
    auto input = input_.retire();
    if (!input) {
        retain(std::move(input.error().primary));
        for (auto& error : input.error().cleanup) retain(std::move(error));
    }
    auto q1_retirement = q1_->retire();
    for (std::size_t index{}; index < q1_retirement.count; ++index)
        retain(std::move(*q1_retirement.failures[index]));
    result.overflowed = result.overflowed || q1_retirement.overflowed;
    historical_.reset();
    return result;
}
auto MonitoringQ2AuthoritySession::bind_fresh_plan(
    const RunnerContextAcquisitionSession& session,
    const MonitoringHostQualificationPlan& host_plan,
    const runtime::AdmittedPlacementRequest& admitted) noexcept
    -> std::expected<MonitoringQ2CollectorPlan, MonitoringQ2AuthorityFailure> {
    if (consumed_)
        std::terminate();
    const auto& planned = host_plan.bandwidth_workers();
    std::vector<MonitoringQ2WorkerBinding> workers;
    std::string running_build_attestation;
    std::optional<std::array<MonitoringQ1GitCommandFailure, 9>> failures;
    try {
        workers.reserve(planned.size());
        running_build_attestation.assign(running_build_.canonical());
        failures.emplace(std::array{wrapped("q2_plan_changed"), wrapped("predecessor_changed"),
                                    wrapped("running_build_changed"), wrapped("worker_count"),
                                    wrapped("page_size_operation"), wrapped("page_capacity"),
                                    wrapped("worker_binding"), wrapped("predecessor_retirement"),
                                    wrapped("contained")});
    } catch (...) {
        core::Error bootstrap{};
        bootstrap.category = core::ErrorCategory::monitoring;
        bootstrap.continuation = core::ContinuationPolicy::abort_command;
        return std::unexpected{MonitoringQ2AuthorityFailure{
            MonitoringQ2AuthorityStage::worker_admission, std::move(bootstrap)}};
    }
    consumed_ = true;
    auto active_stage = MonitoringQ2AuthorityStage::collection_plan;
    const auto take_failure = [&](std::size_t index) { return std::move((*failures)[index]); };
    MonitoringQ1GitCommandFailure late_contained_failure{core::Error{}, {}};
    late_contained_failure.primary.context = nullptr;
    bool running_build_active = true, predecessor_active = true, input_active = true;
    std::optional<MonitoringQ2PageSizeIssuer> page_issuer;
    const auto fail = [&](MonitoringQ1GitCommandFailure result, MonitoringQ2AuthorityStage stage) noexcept {
        const auto retain=[&](core::Error value) noexcept {
            if(cleanup_ledger_.count<cleanup_ledger_.failures.size())
                cleanup_ledger_.failures[cleanup_ledger_.count++].emplace(std::move(value));
            else cleanup_ledger_.overflowed=true;
        };
        if (page_issuer)
            static_cast<void>(page_issuer->retire(false));
        for (auto& item : result.cleanup) retain(std::move(item));
        result.cleanup.clear();
        if (running_build_active) {
            running_build_active = false;
            auto build = running_build_.retire();
            if (!build)
                retain(std::move(build.error()));
        }
        if (predecessor_active) {
            predecessor_active = false;
            auto predecessor = predecessor_.retire();
            if (!predecessor) {
                retain(std::move(predecessor.error().primary));
                for (std::size_t i{}; i < predecessor.error().cleanup_count; ++i)
                    retain(std::move(*predecessor.error().cleanup[i]));
            }
        }
        if (input_active) {
            input_active = false;
            auto input = input_.retire();
            if (!input) {
                retain(std::move(input.error().primary));
                for (auto& e : input.error().cleanup)
                    retain(std::move(e));
            }
        }
        auto q1 = q1_->retire();
        for (std::size_t index{}; index < q1.count; ++index)
            retain(std::move(*q1.failures[index]));
        cleanup_ledger_.overflowed=cleanup_ledger_.overflowed||q1.overflowed;
        historical_.reset();
        MonitoringQ2AuthorityFailure failure{stage,std::move(result.primary)};
        failure.cleanup=std::move(cleanup_ledger_.failures);
        failure.cleanup_count=cleanup_ledger_.count;
        failure.cleanup_overflowed=cleanup_ledger_.overflowed;
        return std::expected<MonitoringQ2CollectorPlan, MonitoringQ2AuthorityFailure>{std::unexpected{std::move(failure)}};
    };
    try {
        auto input_check = input_.recheck();
        if (!input_check)
            return fail(std::move(input_check.error()), MonitoringQ2AuthorityStage::collection_plan);
        if (!*input_check)
            return fail(take_failure(0), MonitoringQ2AuthorityStage::collection_plan);
        active_stage = MonitoringQ2AuthorityStage::predecessor;
        auto predecessor_check = predecessor_.recheck();
        if (!predecessor_check) {
            auto primary = std::move(predecessor_check.error().primary);
            for (std::size_t i{}; i < predecessor_check.error().cleanup_count; ++i)
                if(cleanup_ledger_.count<cleanup_ledger_.failures.size())cleanup_ledger_.failures[cleanup_ledger_.count++].emplace(std::move(*predecessor_check.error().cleanup[i]));else cleanup_ledger_.overflowed=true;
            return fail(MonitoringQ1GitCommandFailure{std::move(primary), {}}, MonitoringQ2AuthorityStage::predecessor);
        }
        if (!*predecessor_check)
            return fail(take_failure(1), MonitoringQ2AuthorityStage::predecessor);
        active_stage = MonitoringQ2AuthorityStage::build_attestation;
        auto build_check = running_build_.recheck();
        if (!build_check)
            return fail(MonitoringQ1GitCommandFailure{std::move(build_check.error()), {}}, MonitoringQ2AuthorityStage::build_attestation);
        if (!*build_check)
            return fail(take_failure(2), MonitoringQ2AuthorityStage::build_attestation);
        active_stage = MonitoringQ2AuthorityStage::worker_admission;
        const auto admitted_workers = admitted.workers();
        if (planned.empty() || planned.size() != admitted_workers.size() ||
            planned.size() > value_.maximum_worker_count)
            return fail(take_failure(3), MonitoringQ2AuthorityStage::worker_admission);
        active_stage = MonitoringQ2AuthorityStage::collection_plan;
        if (!page_operations_.page_size)
            return fail(take_failure(4), MonitoringQ2AuthorityStage::collection_plan);
        const long acquired_page = page_operations_.page_size(page_operations_.context);
        const auto page = host_plan.base_page_size().value();
        if (acquired_page <= 0 || static_cast<std::uint64_t>(acquired_page) != page || page == 0 ||
            value_.ring_data_pages > std::numeric_limits<std::uint64_t>::max() / page - 1U ||
            (value_.ring_data_pages + 1U) * page > value_.raw_in_memory_bytes)
            return fail(take_failure(5), MonitoringQ2AuthorityStage::collection_plan);
        active_stage = MonitoringQ2AuthorityStage::worker_admission;
        for (std::size_t i{}; i < planned.size(); ++i) {
            const auto& p = planned[i];
            const auto& a = admitted_workers[i];
            const auto& cpu = a.request().cpu();
            if (cpu.linux_cpu() != p.linux_cpu || cpu.stable_core_id() != p.stable_core_id ||
                cpu.stable_pu_id() != p.stable_pu_id)
                return fail(take_failure(6), MonitoringQ2AuthorityStage::worker_admission);
            workers.push_back({core::WorkerId{p.worker_id}, p.linux_cpu, a.core(),
                               a.processing_unit(), p.stable_core_id, p.stable_pu_id});
        }
        page_issuer.emplace(MonitoringQ2PageSizeIssuer{workers, page, page_operations_});
        MonitoringQ2CollectorPlan::EvidenceProvenance evidence_provenance{
            std::string{input_.bytes()}, input_.sha256(),
            std::string{predecessor_.historical_q1_commit()},
            predecessor_.predecessor_manifest_sha256(),
            predecessor_.predecessor_checksums_sha256(), predecessor_.q1_evidence_sha256(),
            std::string{predecessor_.q0_profile_repository_commit()},
            std::string{predecessor_.host_environment_digest()}};
        const auto running_build_identity = running_build_.identity();
        active_stage = MonitoringQ2AuthorityStage::build_attestation;
        running_build_active = false;
        auto build_retired = running_build_.retire();
        if (!build_retired)
            return fail(MonitoringQ1GitCommandFailure{std::move(build_retired.error()), {}}, MonitoringQ2AuthorityStage::build_attestation);
        active_stage = MonitoringQ2AuthorityStage::predecessor;
        predecessor_active = false;
        auto predecessor_retired = predecessor_.retire();
        if (!predecessor_retired) {
            auto primary = std::move(predecessor_retired.error().primary);
            for (std::size_t i{}; i < predecessor_retired.error().cleanup_count; ++i)
                if(cleanup_ledger_.count<cleanup_ledger_.failures.size())cleanup_ledger_.failures[cleanup_ledger_.count++].emplace(std::move(*predecessor_retired.error().cleanup[i]));else cleanup_ledger_.overflowed=true;
            return fail(MonitoringQ1GitCommandFailure{std::move(primary), {}}, MonitoringQ2AuthorityStage::predecessor);
        }
        active_stage = MonitoringQ2AuthorityStage::collection_plan;
        input_active = false;
        auto input_retired = input_.retire();
        if (!input_retired) {
            auto result = std::move(input_retired.error());
            return fail(std::move(result), MonitoringQ2AuthorityStage::collection_plan);
        }
        historical_.reset();
        active_stage = MonitoringQ2AuthorityStage::worker_admission;
        failures.reset();
        MonitoringQ2AuthorityFailure q1_failure_out{
            MonitoringQ2AuthorityStage::worker_admission, core::Error{}};
        auto q1 = q1_->bind(session, host_plan, admitted);
        if (!q1) {
            static_cast<void>(page_issuer->retire(false));
            auto failure=std::move(q1.error());
            auto& out = q1_failure_out;
            out.primary = std::move(failure.primary);
            const auto retain = [&](core::Error item) noexcept {
                if (out.cleanup_count < out.cleanup.size())
                    out.cleanup[out.cleanup_count++].emplace(std::move(item));
                else
                    out.cleanup_overflowed = true;
            };
            for (std::size_t index{}; index < cleanup_ledger_.count; ++index)
                retain(std::move(*cleanup_ledger_.failures[index]));
            out.cleanup_overflowed |= cleanup_ledger_.overflowed;
            for (std::size_t index{}; index < failure.cleanup.count; ++index)
                retain(std::move(*failure.cleanup.failures[index]));
            out.cleanup_overflowed |= failure.cleanup.overflowed;
            return std::unexpected{std::move(q1_failure_out)};
        }
        auto worker_snapshot = std::move(*q1).take_worker_snapshot();
        if (!worker_snapshot) {
            static_cast<void>(page_issuer->retire(false));
            auto failure=wrapped("worker_binding");
            MonitoringQ2AuthorityFailure out{MonitoringQ2AuthorityStage::worker_admission,std::move(failure.primary)};
            for(auto& item:failure.cleanup){if(out.cleanup_count<out.cleanup.size())out.cleanup[out.cleanup_count++].emplace(std::move(item));else out.cleanup_overflowed=true;}
            return std::unexpected{std::move(out)};
        }
        MonitoringQ2WorkerSession worker_session{std::move(*worker_snapshot),
                                                 std::move(*page_issuer)};
        return MonitoringQ2CollectorPlan{std::move(*q1),
                                         std::move(value_),
                                         std::move(workers),
                                         page,
                                         std::move(worker_session),
                                         running_build_identity,
                                         std::move(running_build_attestation),
                                         std::move(evidence_provenance)};
    } catch (...) {
        return fail(failures ? take_failure(8) : std::move(late_contained_failure), active_stage);
    }
}
} // namespace epycbench::monitoring::qualification::detail
