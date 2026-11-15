#pragma once

#include "monitoring/host_q4_composition_owner.hpp"
#include "monitoring/host_q4_governed_plan_schema_binding.hpp"
#include "monitoring/host_q4_historical_git_authority.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"

#include <filesystem>
#include <variant>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4RetainedEvidence;
enum class MonitoringQ4ProjectedAuthorityPhase : unsigned char {
    repository,
    collection_plan,
    predecessor,
    build_attestation,
    finalization
};
enum class MonitoringQ4ProjectedPrimaryKind : unsigned char { stage_failure, terminal_containment };
struct MonitoringQ4ProjectedAuthorityFailure final {
    static constexpr std::size_t ordinary_capacity = 31;
    static constexpr std::size_t capacity = ordinary_capacity + 1U;
    MonitoringQ4ProjectedPrimaryKind primary_kind{};
    std::optional<core::Error> primary{};
    MonitoringQ4ProjectedAuthorityPhase phase{};
    std::array<std::optional<core::Error>, capacity> cleanup{};
    std::size_t cleanup_count{};
};

enum class MonitoringQ4AuthorityStage : unsigned char {
    repository,
    current_revision,
    governed_input,
    history,
    predecessor,
    plan_schema,
    running_build,
    final_recheck,
    retirement,
    contained
};

struct MonitoringQ4AuthorityFailure final {
    using Detail = std::variant<std::monostate, MonitoringQ1GitCommandFailure,
                                MonitoringQ4GovernedInputFailure, MonitoringQ4HistoricalGitFailure,
                                MonitoringQ4CompleteQ3AuthorityFailure,
                                MonitoringQ4GovernedPlanSchemaBindingFailure, core::Error>;
    MonitoringQ4AuthorityStage stage{};
    MonitoringQ4CompositionFact primary{};
    MonitoringQ4CompositionRetirement cleanup{};
    Detail detail{};
    std::optional<MonitoringQ4ProjectedAuthorityFailure> projected{};
};

struct MonitoringQ4EvidenceProvenance final {
    core::Sha256Digest profile;
    std::array<core::Sha256Digest, 7> predecessor;
};

// Owning facts captured from the genuine held owners before completed-source
// retirement. The locator and Build bytes do not recreate those authorities.
struct MonitoringQ4CompletedSourceProvenance final {
    std::filesystem::path accepted_repository_path;
    core::detail::GovernedRepositoryRootIdentity repository_identity;
    MonitoringQ1GitExecutableIdentity git_identity;
    core::Sha256Digest current_build_identity;
    std::string current_build_bytes;
};
[[nodiscard]] inline auto monitoring_q4_capture_evidence_provenance(
    core::Sha256Digest profile, const MonitoringQ4CompleteQ3PredecessorValue& predecessor) noexcept
    -> MonitoringQ4EvidenceProvenance {
    return {profile, predecessor.digests()};
}

class MonitoringQ4KernelMaximumReady final {
  public:
    MonitoringQ4KernelMaximumReady(const MonitoringQ4KernelMaximumReady&) = delete;
    MonitoringQ4KernelMaximumReady(MonitoringQ4KernelMaximumReady&&) noexcept = default;
    auto operator=(const MonitoringQ4KernelMaximumReady&)
        -> MonitoringQ4KernelMaximumReady& = delete;
    auto operator=(MonitoringQ4KernelMaximumReady&&) -> MonitoringQ4KernelMaximumReady& = delete;

    [[nodiscard]] auto governed() const noexcept -> const MonitoringQ4GovernedPlanSchemaBinding& {
        return governed_;
    }
    [[nodiscard]] auto history() const noexcept -> const MonitoringQ4HistoricalGitValue& {
        return history_;
    }
    [[nodiscard]] auto admitted_linux_cpus() const noexcept
        -> std::span<const MonitoringQ4AdmittedLinuxCpuId> {
        return projection_.admitted_linux_cpus;
    }
    [[nodiscard]] auto base_page_size_bytes() const noexcept -> MonitoringQ4BasePageSizeBytes {
        return projection_.page_size_bytes;
    }
    [[nodiscard]] auto predecessor_projection() const noexcept
        -> const MonitoringQ4AuthenticatedHostProjection& {
        return projection_;
    }
    [[nodiscard]] auto qualification_id() const noexcept -> std::string_view { return qid_; }
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return head_; }
    [[nodiscard]] auto build_identity() const noexcept -> const core::Sha256Digest& {
        return build_identity_;
    }
    [[nodiscard]] auto build_attestation() const noexcept -> std::string_view {
        return build_attestation_;
    }
    [[nodiscard]] auto evidence_provenance() const noexcept
        -> const MonitoringQ4EvidenceProvenance& { return evidence_provenance_; }

  private:
    MonitoringQ4KernelMaximumReady(MonitoringQ4GovernedPlanSchemaBinding,
                                   MonitoringQ4HistoricalGitValue,
                                   MonitoringQ4AuthenticatedHostProjection, std::string,
                                   std::string, core::Sha256Digest, std::string,
                                   MonitoringQ4EvidenceProvenance,
                                   std::shared_ptr<const void>) noexcept;
    MonitoringQ4GovernedPlanSchemaBinding governed_;
    MonitoringQ4HistoricalGitValue history_;
    MonitoringQ4AuthenticatedHostProjection projection_;
    std::string qid_, head_;
    core::Sha256Digest build_identity_;
    std::string build_attestation_;
    MonitoringQ4EvidenceProvenance evidence_provenance_;
    std::shared_ptr<const void> kernel_maximum_binding_;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4KernelMaximumAuthority;
    friend class MonitoringQ4KernelMaximumAuthorityTestAccess;
    friend class MonitoringQ4SourceReadyTransition;
    friend class MonitoringQ4SourceReadyTestAccess;
    friend class MonitoringQ4CpuFreqInventoryAuthority;
    friend class MonitoringQ4CpuFreqSemanticWorkspace;
};

class MonitoringQ4AuthoritySession final {
  public:
    MonitoringQ4AuthoritySession(const MonitoringQ4AuthoritySession&) = delete;
    MonitoringQ4AuthoritySession(MonitoringQ4AuthoritySession&&) noexcept = default;
    auto operator=(const MonitoringQ4AuthoritySession&) -> MonitoringQ4AuthoritySession& = delete;
    auto operator=(MonitoringQ4AuthoritySession&&) -> MonitoringQ4AuthoritySession& = delete;
    ~MonitoringQ4AuthoritySession() noexcept = default;

    [[nodiscard]] static auto authenticate(const std::filesystem::path&, std::string_view) noexcept
        -> std::expected<MonitoringQ4AuthoritySession, MonitoringQ4AuthorityFailure>;
    [[nodiscard]] auto finalize_before_kernel_max() noexcept
        -> std::expected<MonitoringQ4KernelMaximumReady, MonitoringQ4AuthorityFailure>;

  private:
    static auto authenticate_completed(const std::filesystem::path&, std::string_view,
                                       const HostFrozenSourceLaunchBinding* = nullptr) noexcept
        -> std::expected<MonitoringQ4AuthoritySession, MonitoringQ4AuthorityFailure>;
    static auto authenticate_impl(const std::filesystem::path&, std::string_view, bool,
                                 const MonitoringRunningBuildAuthority::Operations* = nullptr,
                                 const HostFrozenSourceLaunchBinding* = nullptr) noexcept
        -> std::expected<MonitoringQ4AuthoritySession, MonitoringQ4AuthorityFailure>;
    auto finalize_completed(std::optional<MonitoringQ1ValidatedPublicValues>&,
                            std::optional<MonitoringQ4RetainedEvidence>&,
                            std::optional<std::array<std::string, 7>>&,
                            std::optional<MonitoringQ4GovernedInputValue>&,
                            std::optional<MonitoringQ4CompletedSourceProvenance>* = nullptr,
                            std::optional<MonitoringQ4HistoricalGitValue>* = nullptr) noexcept
        -> std::expected<void, MonitoringQ4AuthorityFailure>;
    auto completed_predecessor_for_test() noexcept
        -> MonitoringQ4CompleteQ3PredecessorAuthority*;
    struct AcquisitionOperations final {
        void* context{};
        std::expected<void, MonitoringQ4AuthorityFailure> (*step)(void*, std::size_t){};
        MonitoringQ4AuthorityFailure (*rollback)(void*, MonitoringQ4AuthorityFailure) noexcept {};
    };
    static auto acquire_body(AcquisitionOperations) noexcept
        -> std::expected<void, MonitoringQ4AuthorityFailure>;
    struct FinalizeOperations final {
        void* context{};
        std::expected<bool, MonitoringQ4AuthorityFailure> (*check)(void*, std::size_t) noexcept {};
        MonitoringQ4CompositionRetirement (*retire)(void*) noexcept {};
    };
    static auto finalize_body(FinalizeOperations) noexcept
        -> std::expected<void, MonitoringQ4AuthorityFailure>;
    static auto rollback_owner(MonitoringQ4CompositionOwner&, MonitoringQ4AuthorityFailure) noexcept
        -> MonitoringQ4AuthorityFailure;
    static auto project_retired(MonitoringQ4AuthorityFailure) noexcept
        -> MonitoringQ4AuthorityFailure;
    explicit MonitoringQ4AuthoritySession(bool consumed) noexcept : consumed_{consumed} {}
    static void set_bootstrap_fault(bool) noexcept;
    static void set_source_provenance_checkpoint(void (*)(unsigned char) noexcept) noexcept;
    MonitoringQ4AuthoritySession(MonitoringQ4CompositionOwner, MonitoringQ4HistoricalGitValue,
                                 MonitoringQ4GovernedPlanSchemaBinding,
                                 std::shared_ptr<const void>) noexcept;
    std::optional<MonitoringQ4CompositionOwner> owner_;
    std::optional<MonitoringQ4HistoricalGitValue> history_;
    std::optional<MonitoringQ4GovernedPlanSchemaBinding> binding_;
    std::shared_ptr<const void> kernel_maximum_binding_;
    bool consumed_{};
    bool completed_mode_{};
    friend class HostFrozenSourceAdmission;
    friend class HostFrozenSourceAdmissionTestAccess;
    friend class MonitoringQ4AuthoritySessionTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
