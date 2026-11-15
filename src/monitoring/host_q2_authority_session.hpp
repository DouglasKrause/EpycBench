#pragma once
#include "epycbench/platform_linux/thread_identity.hpp"
#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include "monitoring/host_q2_complete_q1_predecessor_authority.hpp"
#include "monitoring/host_q2_native_worker_team.hpp"
#include "monitoring/host_q2_plan_public_input_authority.hpp"
#include "monitoring/host_q2_plan_value.hpp"
#include "monitoring/host_q2_q1_authority.hpp"
#include "monitoring/host_running_build_authority.hpp"

namespace epycbench::platform_linux::detail {
class Q2PerfRetiredState;
}

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ2AuthoritySessionTestAccess;
class MonitoringQ2ContextSwitchSourceOwner;
class MonitoringQ2PageSizeIssuer;
class MonitoringQ2ControlledEventRuntimeBridge;
class MonitoringQ2QuiescedWorkerBorrow;
class MonitoringQ2ContextSourceFinalization;
class MonitoringQ2ContextSourceFailure;
struct MonitoringQ2RecoveredWorkerFailure;
class MonitoringQ2SourcePreparationWorkerHandoff;
class MonitoringQ2WorkerSession;
class MonitoringQ2ContextSwitchCollector;
class MonitoringQ2TerminalContainment;
struct MonitoringQ2WorkerBinding final {
    core::WorkerId worker;
    core::LinuxCpuId linux_cpu;
    core::CoreId core;
    core::ProcessingUnitId processing_unit;
    topology::StableCoreId stable_core;
    topology::StableProcessingUnitId stable_processing_unit;
};
struct Q2TargetSnapshot final {
    core::WorkerId worker;
    core::LinuxCpuId cpu;
    core::CoreId core;
    core::ProcessingUnitId pu;
    topology::StableCoreId stable_core;
    topology::StableProcessingUnitId stable_pu;
    platform_linux::LinuxThreadId tid;
    bool quiescent;
};
struct Q2RuntimeGenerationOperations {
    void* context{};
    std::expected<void, core::Error> (*begin)(void*, std::size_t,
                                              runtime::detail::TargetedWorkerOperation,
                                              runtime::detail::TargetedWorkerReadinessOperation,
                                              MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<void, core::Error> (*await_ready)(
        void*, MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<void, core::Error> (*release)(void*) noexcept {};
    std::expected<void, core::Error> (*await_timed_completion)(
        void*, MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<void, core::Error> (*await_quiescence)(
        void*, MonitoringQ2DeadlineCancellationContext) noexcept {};
    std::expected<bool, core::Error> (*collect_succeeded)(void*) noexcept {};
    std::expected<void, core::Error> (*cancel_ready)(void*) noexcept {};
    std::expected<void, core::Error> (*reset)(void*) noexcept {};
};
class MonitoringQ2PageSizeRecheck final {
  public:
    MonitoringQ2PageSizeRecheck(MonitoringQ2PageSizeRecheck&&) noexcept;
    MonitoringQ2PageSizeRecheck(const MonitoringQ2PageSizeRecheck&) = delete;
    ~MonitoringQ2PageSizeRecheck() noexcept;

  private:
    struct Operations {
        void* context{};
        long (*page_size)(void*) noexcept {};
    };
    struct State;
    MonitoringQ2PageSizeRecheck(std::uint64_t expected, Operations operations,
                                State* state = nullptr, std::size_t slot = 0) noexcept
        : expected_{expected}, operations_{operations}, state_{state}, slot_{slot} {}
    auto consume() noexcept -> std::expected<std::uint64_t, core::Error>;
    std::uint64_t expected_{};
    Operations operations_{};
    State* state_{};
    std::size_t slot_{};
    bool consumed_{};
    friend class MonitoringQ2AuthoritySession;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2ContextSwitchSourceOwner;
    friend class MonitoringQ2PageSizeIssuer;
    friend class MonitoringQ2SourcePreparationWorkerHandoff;
};
class MonitoringQ2PageSizeIssuer final {
  public:
    MonitoringQ2PageSizeIssuer(MonitoringQ2PageSizeIssuer&&) noexcept;
    MonitoringQ2PageSizeIssuer(const MonitoringQ2PageSizeIssuer&) = delete;
    ~MonitoringQ2PageSizeIssuer() noexcept;

  private:
    MonitoringQ2PageSizeIssuer(std::span<const MonitoringQ2WorkerBinding>, std::uint64_t,
                               MonitoringQ2PageSizeRecheck::Operations);
    auto issue(core::WorkerId) noexcept -> MonitoringQ2PageSizeRecheck;
    void block() noexcept;
    auto slots() const noexcept -> std::size_t;
    auto retire(bool successful) noexcept -> std::expected<void, core::Error>;
    std::unique_ptr<MonitoringQ2PageSizeRecheck::State> state_;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2WorkerSession;
    friend class MonitoringQ2AuthoritySession;
};
class MonitoringQ2SourcePreparationWorkerHandoff final {
  public:
    MonitoringQ2SourcePreparationWorkerHandoff(
        MonitoringQ2SourcePreparationWorkerHandoff&&) noexcept;
    MonitoringQ2SourcePreparationWorkerHandoff(const MonitoringQ2SourcePreparationWorkerHandoff&) =
        delete;
    ~MonitoringQ2SourcePreparationWorkerHandoff() noexcept;
    auto target() const noexcept -> const Q2TargetSnapshot& { return target_; }

  private:
    MonitoringQ2SourcePreparationWorkerHandoff(MonitoringQ2WorkerSession*, std::size_t,
                                               Q2TargetSnapshot, MonitoringQ2PageSizeRecheck&&,
                                               Q2RuntimeGenerationOperations) noexcept;
    auto take_page_recheck() noexcept -> MonitoringQ2PageSizeRecheck;
    void cancel_before_prepare() noexcept;
    void fail() noexcept;
    MonitoringQ2WorkerSession* owner_{};
    std::size_t index_{};
    Q2TargetSnapshot target_;
    std::optional<MonitoringQ2PageSizeRecheck> page_recheck_;
    Q2RuntimeGenerationOperations runtime_operations_{};
    bool consumed_{};
    friend class MonitoringQ2WorkerSession;
    friend class MonitoringQ2ControlledEventRuntimeBridge;
    friend class MonitoringQ2ContextSwitchSourceOwner;
    friend class MonitoringQ2ContextSwitchCollector;
    friend class MonitoringQ2AuthoritySessionTestAccess;
};
class MonitoringQ2WorkerSession final {
  public:
    MonitoringQ2WorkerSession(MonitoringQ2WorkerSession&&) noexcept;
    MonitoringQ2WorkerSession(const MonitoringQ2WorkerSession&) = delete;
    ~MonitoringQ2WorkerSession() noexcept;

  private:
    MonitoringQ2WorkerSession(runtime::detail::MonitoringQ1AdmittedWorkerSnapshot snapshot,
                              MonitoringQ2PageSizeIssuer issuer);
    auto take_snapshot() noexcept -> runtime::detail::MonitoringQ1AdmittedWorkerSnapshot;
    auto activate(MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, core::Error>;
    auto activate_native_with(MonitoringQ2DeadlineCancellationContext,
                              runtime::detail::WorkerTeamDependencies,
                              const MonitoringQ2NativeThreadSystem* = nullptr) noexcept
        -> std::expected<void, core::Error>;
    auto activate_with(runtime::RunnerCancellation&,
                       runtime::detail::WorkerTeamDependencies) noexcept
        -> std::expected<void, core::Error>;
    void activate_injected() noexcept;
    auto borrow(std::size_t) noexcept -> runtime::detail::MonitoringQ1WorkerSession*;
    auto begin_source_preparation(std::size_t, const MonitoringQ2WorkerBinding&) noexcept
        -> std::expected<MonitoringQ2SourcePreparationWorkerHandoff, core::Error>;
    void release(std::size_t, bool) noexcept;
    auto complete_worker(MonitoringQ2QuiescedWorkerBorrow&&,
                         MonitoringQ2ContextSourceFinalization&&,
                         MonitoringQ2DeadlineCancellationContext) noexcept
        -> std::expected<void, core::Error>;
    void complete_worker_failure(MonitoringQ2QuiescedWorkerBorrow&&,
                                 MonitoringQ2ContextSourceFailure&&,
                                 MonitoringQ2DeadlineCancellationContext) noexcept;
    void complete_worker_controlled_failure(MonitoringQ2RecoveredWorkerFailure&&,
                                            MonitoringQ2DeadlineCancellationContext) noexcept;
    void complete_worker_combined_failure(MonitoringQ2RecoveredWorkerFailure&&,
                                          MonitoringQ2ContextSourceFailure&&,
                                          MonitoringQ2DeadlineCancellationContext) noexcept;
    void complete_worker_combined_failure(MonitoringQ2RecoveredWorkerFailure&&,
                                          MonitoringQ2ContextSourceFinalization&&,
                                          MonitoringQ2DeadlineCancellationContext) noexcept;
    struct Finalization final {
        std::optional<core::Error> primary;
        std::vector<core::Error> cleanup;
        bool retirement_retryable{};
    };
    auto finalize(bool, MonitoringQ2DeadlineCancellationContext) noexcept -> Finalization;
    auto take_native_containment_diagnostics() noexcept
        -> MonitoringQ2NativeWorkerTeam::CreateFailure;
    std::optional<runtime::detail::MonitoringQ1AdmittedWorkerSnapshot> snapshot_;
    std::optional<runtime::detail::MonitoringQ1WorkerSession> runtime_;
    std::optional<MonitoringQ2NativeWorkerTeam> native_runtime_;
    MonitoringQ2PageSizeIssuer issuer_;
    std::size_t next_worker_{};
    bool borrowed_{}, blocked_{}, finalized_{};
    bool injected_runtime_{};
    unsigned activation_fault_{};
    std::optional<core::Error> injected_finalization_primary_;
    std::vector<core::Error> injected_finalization_cleanup_;
    std::vector<platform_linux::detail::Q2PerfRetiredState> deferred_source_states_;
    void* post_inactive_context_{};
    void (*post_inactive_hook_)(void*) noexcept {};
    core::Error activation_error_, incomplete_error_, association_error_, overflow_error_;
    std::vector<core::Error> finalization_ledger_;
    bool finalization_overflowed_{};
    bool finalization_started_{};
    bool finalization_successful_{};
    bool injected_retirement_retryable_{};
    std::optional<core::Error> finalization_primary_;
    std::optional<MonitoringQ2NativeWorkerTeam::CreateFailure> native_diagnostics_;
    MonitoringQ2CompactLedger<16> controlled_diagnostics_;
    friend class MonitoringQ2CollectorPlan;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2AuthoritySession;
    friend class MonitoringQ2ControlledEventRuntimeBridge;
    friend class MonitoringQ2QuiescedWorkerBorrow;
    friend class MonitoringQ2SourcePreparationWorkerHandoff;
    friend class MonitoringQ2ContextSwitchCollector;
    friend class MonitoringQ2TerminalContainment;
};
class MonitoringQ2CollectorPlan final {
  public:
    struct EvidenceProvenance final {
        std::string q2_plan_bytes;
        core::Sha256Digest q2_plan_sha256;
        std::string historical_q1_commit;
        core::Sha256Digest predecessor_manifest_sha256;
        core::Sha256Digest predecessor_checksums_sha256;
        core::Sha256Digest q1_evidence_sha256;
        std::string q0_profile_repository_commit;
        std::string host_environment_digest;
    };
    MonitoringQ2CollectorPlan(MonitoringQ2CollectorPlan&&) noexcept;
    MonitoringQ2CollectorPlan(const MonitoringQ2CollectorPlan&) = delete;
    auto workers() const noexcept -> std::span<const MonitoringQ2WorkerBinding> { return workers_; }
    auto page_size_bytes() const noexcept -> std::uint64_t { return page_size_; }
    auto plan() const noexcept -> const MonitoringQ2PlanValue& { return plan_; }
    auto running_build_identity() const noexcept -> const core::Sha256Digest& {
        return running_build_identity_;
    }
    auto running_build_attestation() const noexcept -> std::string_view {
        return running_build_attestation_;
    }
    auto evidence_provenance() const noexcept -> const EvidenceProvenance& {
        if (!provenance_) std::terminate();
        return *provenance_;
    }
    auto qualification_id() const noexcept -> std::string_view { return q1_.qualification_id(); }
    auto repository_commit() const noexcept -> std::string_view { return q1_.repository_commit(); }
    auto profile_sha256() const noexcept -> std::string_view { return q1_.profile_sha256(); }
    auto qualification_plan_digest() const noexcept -> const core::Sha256Digest& {
        return q1_.qualification_plan_digest();
    }
    auto q0_provenance() const noexcept -> const MonitoringQ1EvidenceProvenance& {
        return q1_.evidence_provenance();
    }

  private:
    MonitoringQ2CollectorPlan(MonitoringQ1CollectorPlan q1, MonitoringQ2PlanValue plan,
                              std::vector<MonitoringQ2WorkerBinding> workers, std::uint64_t page,
                              MonitoringQ2WorkerSession worker_session,
                              core::Sha256Digest build_identity,
                              std::string build_attestation,
                              std::optional<EvidenceProvenance> provenance = std::nullopt) noexcept
        : q1_{std::move(q1)}, plan_{std::move(plan)}, workers_{std::move(workers)},
          page_size_{page}, worker_session_{std::move(worker_session)},
          running_build_identity_{build_identity},
          running_build_attestation_{std::move(build_attestation)},
          provenance_{std::move(provenance)} {}
    MonitoringQ1CollectorPlan q1_;
    MonitoringQ2PlanValue plan_;
    std::vector<MonitoringQ2WorkerBinding> workers_;
    std::uint64_t page_size_{};
    std::optional<MonitoringQ2WorkerSession> worker_session_;
    core::Sha256Digest running_build_identity_;
    std::string running_build_attestation_;
    std::optional<EvidenceProvenance> provenance_;
    friend class MonitoringQ2AuthoritySession;
    friend class MonitoringQ2AuthoritySessionTestAccess;
    friend class MonitoringQ2ContextSwitchCollector;
    friend class MonitoringQ2TerminalContainment;
    friend class MonitoringQ2EvidenceTestAccess;
};
enum class MonitoringQ2AuthorityStage : std::uint8_t {
    repository, collection_plan, predecessor, build_attestation, worker_admission
};
struct MonitoringQ2AuthorityFailure final {
    MonitoringQ2AuthorityStage stage{MonitoringQ2AuthorityStage::repository};
    core::Error primary;
    std::array<std::optional<core::Error>, 63> cleanup{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
};
class MonitoringQ2AuthoritySession final {
  public:
    MonitoringQ2AuthoritySession(MonitoringQ2AuthoritySession&&) noexcept;
    MonitoringQ2AuthoritySession(const MonitoringQ2AuthoritySession&) = delete;
    ~MonitoringQ2AuthoritySession() noexcept = default;
    static auto authenticate(const std::filesystem::path&, std::string_view) noexcept
        -> std::expected<MonitoringQ2AuthoritySession, MonitoringQ2AuthorityFailure>;
    [[nodiscard]] auto validated_build_attestation() const noexcept -> std::string_view;
    [[nodiscard]] auto authenticated_repository_commit() const noexcept -> std::string_view;
    struct PrebindRetirement final {
        std::array<std::optional<core::Error>, 63> errors{};
        std::size_t count{};
        bool overflowed{};
    };
    auto retire_before_binding() noexcept -> PrebindRetirement;
    auto bind_fresh_plan(const RunnerContextAcquisitionSession&,
                         const MonitoringHostQualificationPlan&,
                         const runtime::AdmittedPlacementRequest&) noexcept
        -> std::expected<MonitoringQ2CollectorPlan, MonitoringQ2AuthorityFailure>;

  private:
    struct HistoricalContext final {
        void* q1_impl{};
        std::expected<void, MonitoringQ2HistoricalFailure> (*authenticate)(void*, std::string_view,
                                                                           std::string_view,
                                                                           std::string_view,
                                                                           std::string_view,
                                                                           std::string_view){};
    };
    static auto authenticate_with(const std::filesystem::path&, std::string_view,
                                  MonitoringRunningBuildAuthority::Operations) noexcept
        -> std::expected<MonitoringQ2AuthoritySession, MonitoringQ2AuthorityFailure>;
    MonitoringQ2AuthoritySession(std::unique_ptr<MonitoringQ2Q1Authority> q1,
                                 MonitoringQ2PlanPublicInputAuthority input,
                                 MonitoringQ2PlanValue value,
                                 MonitoringQ2CompleteQ1PredecessorAuthority predecessor,
                                 MonitoringRunningBuildAuthority running_build,
                                 std::unique_ptr<HistoricalContext> historical,
                                 MonitoringQ2PageSizeRecheck::Operations page_operations,
                                 MonitoringQ1BoundedRetirement cleanup_ledger) noexcept;
    std::unique_ptr<MonitoringQ2Q1Authority> q1_;
    MonitoringQ2PlanPublicInputAuthority input_;
    MonitoringQ2PlanValue value_;
    MonitoringQ2CompleteQ1PredecessorAuthority predecessor_;
    MonitoringRunningBuildAuthority running_build_;
    std::unique_ptr<HistoricalContext> historical_;
    bool consumed_{};
    MonitoringQ2PageSizeRecheck::Operations page_operations_{};
    MonitoringQ1BoundedRetirement cleanup_ledger_;
    friend class MonitoringQ2AuthoritySessionTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
