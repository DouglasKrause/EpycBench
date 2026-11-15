#pragma once

#include "monitoring/host_q3_complete_q2_predecessor_authority.hpp"
#include "monitoring/host_q1_plan_v2_authority_session.hpp"
#include "monitoring/host_q3_plan_public_input_authority.hpp"
#include "monitoring/host_running_build_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
enum class MonitoringQ3AuthorityStage : std::uint8_t {
    repository, collection_plan, predecessor, build_attestation, final_recheck, retirement
};
struct MonitoringQ3AuthorityFailure final {
    MonitoringQ3AuthorityFailure()=default;
    MonitoringQ3AuthorityFailure(MonitoringQ3AuthorityStage value,core::Error error,
      MonitoringQ1BoundedRetirement retired):stage(value),primary(std::move(error)),cleanup(std::move(retired)){}
    MonitoringQ3AuthorityStage stage{MonitoringQ3AuthorityStage::repository};
    core::Error primary;
    MonitoringQ1BoundedRetirement cleanup;
    std::optional<MonitoringGovernedPlanAuthoredDiagnostic> authored;
};
class MonitoringQ3SourceReady final {
  public:
    MonitoringQ3SourceReady(const MonitoringQ3SourceReady&) = delete;
    MonitoringQ3SourceReady(MonitoringQ3SourceReady&&) noexcept = default;
    [[nodiscard]] auto plan() const noexcept -> const MonitoringQ3PlanValue& { return plan_; }
    [[nodiscard]] auto predecessor() const noexcept
        -> const MonitoringQ3CompleteQ2PredecessorValue& { return predecessor_; }
    [[nodiscard]] auto repository_commit() const noexcept -> std::string_view { return commit_; }
    [[nodiscard]] auto qualification_id() const noexcept -> std::string_view { return qualification_id_; }
    [[nodiscard]] auto profile_sha256() const noexcept -> std::string_view { return profile_sha256_; }
    [[nodiscard]] auto plan_sha256() const noexcept -> const core::Sha256Digest& { return plan_sha256_; }
    [[nodiscard]] auto plan_bytes() const noexcept -> std::string_view { return plan_bytes_; }
    [[nodiscard]] auto build_identity() const noexcept -> const core::Sha256Digest& { return build_identity_; }
    [[nodiscard]] auto build_attestation() const noexcept -> std::string_view { return build_attestation_; }
  private:
    MonitoringQ3SourceReady(MonitoringQ3PlanValue, MonitoringQ3CompleteQ2PredecessorValue,
        std::string, std::string, std::string, std::string, core::Sha256Digest,
        core::Sha256Digest, std::string) noexcept;
    MonitoringQ3PlanValue plan_;
    MonitoringQ3CompleteQ2PredecessorValue predecessor_;
    std::string commit_, qualification_id_, profile_sha256_, plan_bytes_;
    core::Sha256Digest plan_sha256_, build_identity_;
    std::string build_attestation_;
    friend class MonitoringQ3AuthoritySession;
    friend class MonitoringQ3AuthoritySessionTestAccess;
};
class MonitoringQ3AuthoritySession final {
  public:
    MonitoringQ3AuthoritySession(const MonitoringQ3AuthoritySession&) = delete;
    MonitoringQ3AuthoritySession(MonitoringQ3AuthoritySession&&) noexcept;
    ~MonitoringQ3AuthoritySession() noexcept;
    [[nodiscard]] static auto authenticate(const std::filesystem::path&, std::string_view) noexcept
        -> std::expected<MonitoringQ3AuthoritySession, MonitoringQ3AuthorityFailure>;
    [[nodiscard]] auto finalize_before_source() noexcept
        -> std::expected<MonitoringQ3SourceReady, MonitoringQ3AuthorityFailure>;
  private:
    struct AcquisitionOperations final {
        void* context{};
        std::expected<void, MonitoringQ3AuthorityFailure> (*repository)(void*){};
        std::expected<void, MonitoringQ3AuthorityFailure> (*fixed_records)(void*){};
        std::expected<void, MonitoringQ3AuthorityFailure> (*input)(void*){};
        std::expected<void, MonitoringQ3AuthorityFailure> (*decode)(void*){};
        std::expected<void, MonitoringQ3AuthorityFailure> (*q2_binding)(void*){};
        std::expected<void, MonitoringQ3AuthorityFailure> (*predecessor)(void*){};
        std::expected<void, MonitoringQ3AuthorityFailure> (*build)(void*){};
        MonitoringQ3AuthorityFailure (*rollback)(void*, MonitoringQ3AuthorityFailure) noexcept{};
    };
    [[nodiscard]] static auto acquire_lifecycle(AcquisitionOperations) noexcept
        -> std::expected<void, MonitoringQ3AuthorityFailure>;
    [[nodiscard]] static auto typed_failure(MonitoringQ1GitCommandFailure,
        MonitoringQ3AuthorityStage) -> MonitoringQ3AuthorityFailure;
    struct LifecycleOperations final {
        void* context{};
        std::expected<bool, MonitoringQ1BoundedFailure> (*recheck_repository)(void*) noexcept{};
        std::expected<bool, MonitoringQ1BoundedFailure> (*recheck_input)(void*) noexcept{};
        std::expected<bool, MonitoringQ1BoundedFailure> (*recheck_predecessor)(void*) noexcept{};
        std::expected<bool, MonitoringQ1BoundedFailure> (*recheck_build)(void*) noexcept{};
        std::expected<void, MonitoringQ1BoundedFailure> (*retire_build)(void*) noexcept{};
        std::expected<void, MonitoringQ1BoundedFailure> (*retire_predecessor)(void*) noexcept{};
        std::expected<void, MonitoringQ1BoundedFailure> (*retire_input)(void*) noexcept{};
        std::expected<void, MonitoringQ1BoundedFailure> (*retire_repository)(void*) noexcept{};
    };
    [[nodiscard]] static auto finalize_lifecycle(LifecycleOperations,
        MonitoringQ1BoundedRetirement) noexcept
        -> std::expected<void, MonitoringQ3AuthorityFailure>;
    class HistoricalContext final { public: void* repository{}; };
    MonitoringQ3AuthoritySession(MonitoringQ1PlanV2AuthoritySession,
        MonitoringQ3PlanPublicInputAuthority, MonitoringQ3PlanValue,
        MonitoringQ3CompleteQ2PredecessorAuthority, MonitoringRunningBuildAuthority,
        std::unique_ptr<HistoricalContext>, MonitoringQ1BoundedRetirement, std::string,
        std::string) noexcept;
    MonitoringQ3AuthoritySession(LifecycleOperations, MonitoringQ3SourceReady) noexcept;
    std::optional<MonitoringQ1PlanV2AuthoritySession> repository_;
    std::optional<MonitoringQ3PlanPublicInputAuthority> input_;
    MonitoringQ3PlanValue plan_;
    std::optional<MonitoringQ3CompleteQ2PredecessorAuthority> predecessor_;
    std::optional<MonitoringRunningBuildAuthority> build_;
    std::unique_ptr<HistoricalContext> historical_;
    MonitoringQ1BoundedRetirement cleanup_;
    std::string profile_sha256_;
    std::string qualification_id_;
    bool consumed_{};
    std::optional<LifecycleOperations> test_lifecycle_;
    std::optional<MonitoringQ3SourceReady> test_ready_;
    friend class MonitoringQ3AuthoritySessionTestAccess;
};
class MonitoringQ3AuthoritySessionTestAccess final {
  public:
    using LifecycleOperations = MonitoringQ3AuthoritySession::LifecycleOperations;
    using AcquisitionOperations = MonitoringQ3AuthoritySession::AcquisitionOperations;
    static auto finalize(LifecycleOperations operations,
                         MonitoringQ1BoundedRetirement cleanup = {}) noexcept {
        return MonitoringQ3AuthoritySession::finalize_lifecycle(operations, std::move(cleanup));
    }
    static auto acquire(AcquisitionOperations operations) noexcept {
        return MonitoringQ3AuthoritySession::acquire_lifecycle(operations);
    }
    static auto typed(MonitoringQ1GitCommandFailure failure,
                      MonitoringQ3AuthorityStage stage) {
        return MonitoringQ3AuthoritySession::typed_failure(std::move(failure), stage);
    }
    static auto session(LifecycleOperations operations) {
        const auto digest = core::sha256(std::string_view{});
        MonitoringQ3CompleteQ2PredecessorValue predecessor{"q0", "q1", "q2", "host",
            "plan", digest, digest, digest, digest, digest, false};
        MonitoringQ3SourceReady ready{MonitoringQ3PlanValue{}, std::move(predecessor), "commit",
            "qid", "profile", "plan", digest, digest, "build"};
        return MonitoringQ3AuthoritySession{operations, std::move(ready)};
    }
    static auto ready() {
        auto session_value=session({});
        session_value.consumed_=true;
        return std::move(*session_value.test_ready_);
    }
};
} // namespace epycbench::monitoring::qualification::detail
