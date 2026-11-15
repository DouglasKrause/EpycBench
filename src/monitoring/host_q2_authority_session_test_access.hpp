#pragma once
#include "monitoring/host_q2_authority_session.hpp"
#include "monitoring/host_q2_context_switch_source.hpp"
#include "monitoring/host_q2_controlled_event_runtime_bridge.hpp"
#include <cerrno>
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2AuthoritySessionTestAccess final {
  public:
    static void set_post_inactive_hook(MonitoringQ2WorkerSession& session, void* context,
                                       void (*hook)(void*) noexcept) noexcept {
        session.post_inactive_context_ = context;
        session.post_inactive_hook_ = hook;
    }
    static void force_retirement_retryable(MonitoringQ2WorkerSession& session,
                                           int native_error = ETIMEDOUT) noexcept {
        session.injected_retirement_retryable_ = true;
        session.native_diagnostics_.emplace(
            MonitoringQ2CompactDiagnostic{MonitoringQ2CompactOperation::join, native_error});
    }
    using PageOperations = MonitoringQ2PageSizeRecheck::Operations;
    static auto page_token(std::uint64_t expected, PageOperations operations) {
        return MonitoringQ2PageSizeRecheck{expected, operations};
    }
    static auto page_issuer(std::span<const MonitoringQ2WorkerBinding> workers,
                            std::uint64_t expected, PageOperations operations) {
        return MonitoringQ2PageSizeIssuer{workers, expected, operations};
    }
    static auto issue(MonitoringQ2PageSizeIssuer& issuer, core::WorkerId worker) noexcept {
        return issuer.issue(worker);
    }
    static auto retire(MonitoringQ2PageSizeIssuer& issuer, bool successful) noexcept {
        return issuer.retire(successful);
    }
    static auto consume(MonitoringQ2PageSizeRecheck& token) noexcept { return token.consume(); }
    static auto take_worker_session(MonitoringQ2CollectorPlan& plan) noexcept {
        if (!plan.worker_session_)
            std::terminate();
        auto result = std::move(*plan.worker_session_);
        plan.worker_session_.reset();
        return result;
    }
    static auto worker_session_ref(MonitoringQ2CollectorPlan& plan) noexcept
        -> MonitoringQ2WorkerSession& {
        if (!plan.worker_session_)
            std::terminate();
        return *plan.worker_session_;
    }
    static auto issue(MonitoringQ2WorkerSession& session, core::WorkerId worker) noexcept {
        return session.issuer_.issue(worker);
    }
    static auto begin_source_preparation(MonitoringQ2WorkerSession& session, std::size_t index,
                                         const MonitoringQ2WorkerBinding& binding) noexcept {
        return session.begin_source_preparation(index, binding);
    }
    static auto source_preparation_handoff(MonitoringQ2WorkerSession& session, std::size_t index,
                                           Q2TargetSnapshot target,
                                           Q2RuntimeGenerationOperations operations = {}) {
        static_cast<void>(session.borrow(index));
        auto page = session.issuer_.issue(target.worker);
        return MonitoringQ2SourcePreparationWorkerHandoff{&session, index, std::move(target),
                                                          std::move(page), operations};
    }
    static auto take_page_recheck(MonitoringQ2SourcePreparationWorkerHandoff& handoff) noexcept {
        return handoff.take_page_recheck();
    }
    static auto
    adopt_source_preparation(MonitoringQ2SourcePreparationWorkerHandoff&& handoff) noexcept {
        return MonitoringQ2ControlledEventRuntimeBridge::adopt_existing_borrow(std::move(handoff));
    }
    static auto take_containment_failure(MonitoringQ2BridgeRetirementRecovery& recovery) noexcept {
        return recovery.take_failure_for_containment();
    }
    static auto containment_failure(const MonitoringQ2BridgeRetirementRecovery& recovery) noexcept
        -> const Q2ControlledFailure& {
        return recovery.failure();
    }
    static auto retry_containment(MonitoringQ2BridgeRetirementRecovery& recovery,
                                  MonitoringQ2DeadlineCancellationContext wait) noexcept {
        return recovery.retry(wait);
    }
    static auto retire(MonitoringQ2WorkerSession& session, bool successful) noexcept {
        return session.issuer_.retire(successful);
    }
    static auto take_snapshot(MonitoringQ2WorkerSession& session) noexcept {
        return session.take_snapshot();
    }
    static auto activate(MonitoringQ2WorkerSession& session,
                         runtime::RunnerCancellation& cancellation,
                         runtime::detail::WorkerTeamDependencies dependencies) noexcept {
        return session.activate_with(cancellation, std::move(dependencies));
    }
    static auto activate_native(MonitoringQ2WorkerSession& session,
                                MonitoringQ2DeadlineCancellationContext wait,
                                runtime::detail::WorkerTeamDependencies dependencies,
                                const MonitoringQ2NativeThreadSystem* system = nullptr) noexcept {
        return session.activate_native_with(wait, std::move(dependencies), system);
    }
    static auto native_operations(MonitoringQ2WorkerSession& session) noexcept {
        if (!session.native_runtime_)
            std::terminate();
        return session.native_runtime_->generation_operations();
    }
    static auto finalize(MonitoringQ2WorkerSession& session, bool successful,
                         MonitoringQ2DeadlineCancellationContext wait) noexcept {
        return session.finalize(successful, wait);
    }
    static void activate_injected(MonitoringQ2WorkerSession& session) noexcept {
        session.activate_injected();
    }
    static void activation_fault(MonitoringQ2WorkerSession& session, unsigned fault) noexcept {
        session.activation_fault_ = fault;
    }
    static void borrow(MonitoringQ2WorkerSession& session, std::size_t index) noexcept {
        static_cast<void>(session.borrow(index));
    }
    static void release(MonitoringQ2WorkerSession& session, std::size_t index,
                        bool completed) noexcept {
        session.release(index, completed);
    }
    static auto source_finalized(const MonitoringQ2WorkerBinding& binding,
                                 platform_linux::LinuxThreadId tid, std::uint64_t start = 1,
                                 std::uint64_t end = 2) {
        return MonitoringQ2ContextSourceFinalization{start, end, 0, 2, binding, tid};
    }
    static auto complete(MonitoringQ2WorkerSession& session,
                         MonitoringQ2QuiescedWorkerBorrow&& borrow,
                         MonitoringQ2ContextSourceFinalization&& source,
                         MonitoringQ2DeadlineCancellationContext wait) noexcept {
        return session.complete_worker(std::move(borrow), std::move(source), wait);
    }
    static auto source_failed(const MonitoringQ2WorkerBinding& binding,
                              platform_linux::LinuxThreadId tid, core::Error primary,
                              std::optional<core::Error> cleanup = {}) {
        std::array<std::optional<core::Error>, 2> ledger{};
        std::size_t count{};
        if (cleanup)
            ledger[count++] = std::move(cleanup);
        return MonitoringQ2ContextSourceFailure{std::move(primary), std::move(ledger), count,
                                                binding, tid};
    }
    static void discard(MonitoringQ2ContextSwitchSourceOwner::Terminalization&& terminal) noexcept {
        std::visit([](auto& token) { token.consumed_ = true; }, terminal);
    }
    static auto failure_primary(const MonitoringQ2ContextSourceFailure& failure) noexcept
        -> const core::Error& {
        return failure.primary_;
    }
    static auto failure_cleanup(const MonitoringQ2ContextSourceFailure& failure) noexcept
        -> std::span<const std::optional<core::Error>> {
        return {failure.cleanup_.data(), failure.cleanup_count_};
    }
    static void complete_failed(MonitoringQ2WorkerSession& session,
                                MonitoringQ2QuiescedWorkerBorrow&& borrow,
                                MonitoringQ2ContextSourceFailure&& source,
                                MonitoringQ2DeadlineCancellationContext wait) noexcept {
        session.complete_worker_failure(std::move(borrow), std::move(source), wait);
    }
    static void complete_controlled_failed(MonitoringQ2WorkerSession& session,
                                           MonitoringQ2RecoveredWorkerFailure&& recovered,
                                           MonitoringQ2DeadlineCancellationContext wait) noexcept {
        session.complete_worker_controlled_failure(std::move(recovered), wait);
    }
    static auto controlled_borrow(MonitoringQ2WorkerSession& session, std::size_t index,
                                  Q2TargetSnapshot target,
                                  Q2RuntimeGenerationOperations operations) noexcept {
        return MonitoringQ2ControlledEventRuntimeBridge::create_borrow_injected(
            session, index, std::move(target), operations);
    }
    static auto quiesced(MonitoringQ2WorkerSession& session, std::size_t index,
                         Q2TargetSnapshot target, Q2TargetOperations operations) {
        return MonitoringQ2QuiescedWorkerBorrow{&session, index, std::move(target), operations, {}};
    }
    static void finalization_failure(MonitoringQ2WorkerSession& session, core::Error primary,
                                     std::vector<core::Error> cleanup = {}) {
        session.injected_finalization_primary_ = std::move(primary);
        session.injected_finalization_cleanup_ = std::move(cleanup);
    }
    static auto worker_session(runtime::detail::MonitoringQ1AdmittedWorkerSnapshot snapshot,
                               std::span<const MonitoringQ2WorkerBinding> workers,
                               std::uint64_t page, PageOperations operations) {
        return MonitoringQ2WorkerSession{std::move(snapshot),
                                         MonitoringQ2PageSizeIssuer{workers, page, operations}};
    }
    static auto worker_session(std::span<const MonitoringQ2WorkerBinding> workers,
                               std::uint64_t page, PageOperations operations) {
        return worker_session(admitted_snapshot(workers), workers, page, operations);
    }
    static auto collector_plan(MonitoringQ1CollectorPlan q1, MonitoringQ2PlanValue value,
                               std::vector<MonitoringQ2WorkerBinding> workers, std::uint64_t page,
                               MonitoringQ2WorkerSession session) {
        auto digest = core::Sha256Digest::parse(
            "0000000000000000000000000000000000000000000000000000000000000000");
        if (!digest)
            std::terminate();
        return MonitoringQ2CollectorPlan{std::move(q1), std::move(value),   std::move(workers),
                                         page,          std::move(session), *digest,
                                         "{\"build_identity\":\"" + digest->to_hex() + "\"}"};
    }
    static auto admitted_snapshot(std::span<const MonitoringQ2WorkerBinding> workers)
        -> runtime::detail::MonitoringQ1AdmittedWorkerSnapshot {
        std::vector<runtime::detail::WorkerLaunchSpec> records;
        records.reserve(workers.size());
        for (std::size_t index{}; index < workers.size(); ++index) {
            const auto& worker = workers[index];
            records.push_back({index, worker.worker, worker.processing_unit, worker.core,
                               worker.stable_processing_unit, worker.stable_core,
                               worker.linux_cpu});
        }
        return runtime::detail::MonitoringQ1AdmittedWorkerSnapshot{std::move(records)};
    }
    static auto authenticate(const std::filesystem::path& root, std::string_view qid,
                             MonitoringRunningBuildAuthority::Operations operations) noexcept {
        return MonitoringQ2AuthoritySession::authenticate_with(root, qid, operations);
    }
    static auto session(std::unique_ptr<MonitoringQ2Q1Authority> q1,
                        MonitoringQ2PlanPublicInputAuthority input, MonitoringQ2PlanValue value,
                        MonitoringQ2CompleteQ1PredecessorAuthority predecessor,
                        MonitoringRunningBuildAuthority build, PageOperations page,
                        std::vector<core::Error> cleanup = {}) {
        MonitoringQ1BoundedRetirement bounded;
        for (auto& item : cleanup) {
            if (bounded.count < bounded.failures.size())
                bounded.failures[bounded.count++].emplace(std::move(item));
            else
                bounded.overflowed = true;
        }
        return MonitoringQ2AuthoritySession{std::move(q1),
                                            std::move(input),
                                            std::move(value),
                                            std::move(predecessor),
                                            std::move(build),
                                            nullptr,
                                            page,
                                            std::move(bounded)};
    }
};
} // namespace epycbench::monitoring::qualification::detail
