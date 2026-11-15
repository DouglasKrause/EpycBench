#pragma once
#include "monitoring/host_q1_plan_v2_authority_session.hpp"
namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ1AuthorityBoundary : unsigned int {
    root_retain,
    executable_retain,
    metadata_retain,
    git_capture,
    public_inputs_retain,
    public_values_validate,
    q0_retain,
    authenticate_initial_checkpoint,
    authenticate_replay,
    authenticate_final_checkpoint,
    bind_opaque,
    bind_context,
    bind_provenance,
    bind_digest,
    bind_count,
    bind_ordered_workers,
    bind_revalidate,
    bind_initial_checkpoint,
    bind_replay,
    bind_final_checkpoint,
    retire
};
struct MonitoringQ1AuthorityRetirementResult final {
    MonitoringQ1BoundedRetirement retirement;
};
struct MonitoringQ1AuthorityRetirementOperations final {
    void* context{};
    std::expected<void, MonitoringQ1GitCommandFailure> (*q0)(void*);
    std::expected<void, MonitoringQ1GitCommandFailure> (*inputs)(void*);
    std::expected<void, MonitoringQ1GitCommandFailure> (*metadata)(void*);
    std::expected<void, MonitoringQ1GitCommandFailure> (*executable)(void*);
    std::expected<void, MonitoringQ1GitCommandFailure> (*root)(void*);
};
struct MonitoringQ1ReplayRetirementOperation final {
    void* context{};
    std::expected<void, MonitoringQ1GitCommandFailure> (*retire)(void*);
};
struct MonitoringQ1ReplayCloseResult final {
    bool succeeded{};
    std::optional<MonitoringQ1GitCommandFailure> failure;
};
auto
    aggregate_monitoring_q1_authority_retirement(MonitoringQ1AuthorityRetirementOperations) noexcept
    -> MonitoringQ1AuthorityRetirementResult;
auto aggregate_monitoring_q1_replay_retirement(MonitoringQ1GitCommandFailure,
                                               MonitoringQ1ReplayRetirementOperation) noexcept
    -> MonitoringQ1GitCommandFailure;
auto close_monitoring_q1_replay(MonitoringQ1ReplayRetirementOperation) noexcept
    -> MonitoringQ1ReplayCloseResult;
class MonitoringQ1PlanV2AuthoritySessionTestAccess final {
  public:
    using BoundaryHook = void (*)(void*, MonitoringQ1AuthorityBoundary);
    static void install_hook(void*, BoundaryHook) noexcept;
    static void clear_hook() noexcept;
    static auto inactive() -> MonitoringQ1PlanV2AuthoritySession;
    static auto authenticate_q2_shared(const std::filesystem::path& root,std::string_view qid) noexcept {
        return MonitoringQ1PlanV2AuthoritySession::authenticate_shared(root,qid,true);
    }
    static void require_active(const MonitoringQ1PlanV2AuthoritySession&) noexcept;
    static auto collector(std::string qid, std::string commit, std::string profile,
                          std::string plan, core::Sha256Digest digest,
                          std::vector<core::WorkerId> workers, std::uint64_t n = 512,
                          std::uint64_t capacity = 2173,
                          std::uint64_t total = 4096) -> MonitoringQ1CollectorPlan {
        return {std::move(qid),
                std::move(commit),
                std::move(profile),
                std::move(plan),
                digest,
                std::move(workers),
                n,
                capacity,
                total,
                530,
                17827077,
                MonitoringQ1PlanDerivationMode::default_mode,
                100,
                120};
    }
    static void evidence_provenance(MonitoringQ1CollectorPlan& plan,
                                    MonitoringQ1EvidenceProvenance provenance) noexcept {
        plan.provenance_ = std::move(provenance);
    }
    static auto bound_collector(std::string qid, std::string commit, std::string profile,
                                std::string plan, core::Sha256Digest digest,
                                std::vector<core::WorkerId> workers,
                                const runtime::AdmittedPlacementRequest& admitted,
                                std::uint64_t n = 512, std::uint64_t capacity = 2173)
        -> MonitoringQ1CollectorPlan {
        auto value = collector(std::move(qid), std::move(commit), std::move(profile),
                               std::move(plan), digest, std::move(workers), n, capacity,
                               static_cast<std::uint64_t>(admitted.workers().size()) * n);
        auto snapshot = runtime::detail::project_monitoring_q1_worker_snapshot(admitted);
        if (!snapshot) std::terminate();
        value.worker_snapshot_.emplace(std::move(*snapshot));
        return value;
    }
    static void set_provenance(MonitoringQ1CollectorPlan& value,
                               MonitoringQ1EvidenceProvenance provenance) {
        value.provenance_ = std::move(provenance);
    }
};
} // namespace epycbench::monitoring::qualification::detail
