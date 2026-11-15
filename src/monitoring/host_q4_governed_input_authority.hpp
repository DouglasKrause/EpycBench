#pragma once

#include "monitoring/governed_tracked_bundle.hpp"

namespace epycbench::monitoring::qualification::detail {

class MonitoringQ4AuthoritySession;
class MonitoringQ4HistoricalGitAuthority;
class MonitoringQ4CompleteQ3PredecessorAuthenticator;
class MonitoringQ4GovernedPlanSchemaBinding;
class MonitoringQ4CompositionOwner;

struct MonitoringQ4GovernedArtifact final {
    std::string bytes;
    core::Sha256Digest sha256;
};
using MonitoringQ4GovernedArtifactSlots =
    std::array<std::optional<MonitoringQ4GovernedArtifact>, 18>;

class MonitoringQ4GovernedInputValue final {
  public:
    MonitoringQ4GovernedInputValue(const MonitoringQ4GovernedInputValue&) = delete;
    MonitoringQ4GovernedInputValue(MonitoringQ4GovernedInputValue&&) noexcept = default;
    auto operator=(const MonitoringQ4GovernedInputValue&)
        -> MonitoringQ4GovernedInputValue& = delete;
    auto operator=(MonitoringQ4GovernedInputValue&&) -> MonitoringQ4GovernedInputValue& = delete;

    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return head_; }
    [[nodiscard]] auto profile() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q1_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_plan_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_error_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_authored_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_plan_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_evidence_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_plan_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_evidence_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_error_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_authored_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto current_q2_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;

  private:
    MonitoringQ4GovernedInputValue(MonitoringQ4GovernedArtifactSlots, std::string) noexcept;
    MonitoringQ4GovernedArtifactSlots records_;
    std::string head_;
    friend class MonitoringQ4GovernedInputAuthority;
    friend class MonitoringQ4GovernedInputAuthorityTestAccess;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4HistoricalGitAuthority;
    friend class MonitoringQ4HistoricalGitAuthorityTestAccess;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticator;
};

class MonitoringQ4GovernedInputCandidate final {
  public:
    MonitoringQ4GovernedInputCandidate(const MonitoringQ4GovernedInputCandidate&) = delete;
    MonitoringQ4GovernedInputCandidate(MonitoringQ4GovernedInputCandidate&&) noexcept;
    auto operator=(const MonitoringQ4GovernedInputCandidate&)
        -> MonitoringQ4GovernedInputCandidate& = delete;
    auto operator=(MonitoringQ4GovernedInputCandidate&&)
        -> MonitoringQ4GovernedInputCandidate& = delete;
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return head_; }
    [[nodiscard]] auto profile() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q1_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_plan_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_error_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q2_authored_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_plan_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_evidence_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q3_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_plan() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_plan_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_evidence_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_error_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto q4_authored_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;
    [[nodiscard]] auto current_q2_result_schema() const noexcept -> const MonitoringQ4GovernedArtifact&;

  private:
    MonitoringQ4GovernedInputCandidate(MonitoringQ4GovernedArtifactSlots records, std::string head,
                                       std::shared_ptr<const void> binding) noexcept
        : records_{std::move(records)}, head_{std::move(head)}, binding_{std::move(binding)} {}
    MonitoringQ4GovernedArtifactSlots records_;
    std::string head_;
    std::shared_ptr<const void> binding_;
    bool valid_{true};
    friend class MonitoringQ4GovernedInputAuthority;
    friend class MonitoringQ4GovernedInputAuthorityTestAccess;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4HistoricalGitAuthority;
    friend class MonitoringQ4HistoricalGitAuthorityTestAccess;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticator;
    friend class MonitoringQ4GovernedPlanSchemaBinding;
    friend class MonitoringQ4CompositionOwner;
};

enum class MonitoringQ4GovernedInputFailureStage : unsigned char {
    authentication,
    recheck,
    retirement,
    contained
};
struct MonitoringQ4GovernedInputFailure final {
    MonitoringQ4GovernedInputFailureStage stage{};
    MonitoringGovernedTrackedBundleFailureKind kind{};
    int native_error{};
    MonitoringGovernedTrackedBundleRetirement retirement{};
};

class MonitoringQ4GovernedInputAuthority final {
  public:
    MonitoringQ4GovernedInputAuthority(const MonitoringQ4GovernedInputAuthority&) = delete;
    MonitoringQ4GovernedInputAuthority(MonitoringQ4GovernedInputAuthority&&) noexcept;
    auto operator=(const MonitoringQ4GovernedInputAuthority&)
        -> MonitoringQ4GovernedInputAuthority& = delete;
    auto operator=(MonitoringQ4GovernedInputAuthority&&)
        -> MonitoringQ4GovernedInputAuthority& = delete;

    [[nodiscard]] auto freeze()
        -> std::expected<MonitoringQ4GovernedInputValue, MonitoringQ4GovernedInputFailure>;

  private:
    using Operations = MonitoringGovernedTrackedBundleCore::Operations;
    [[nodiscard]] static auto authenticate(int, MonitoringCurrentRevisionTrackedCapture&&)
        -> std::expected<MonitoringQ4GovernedInputAuthority, MonitoringQ4GovernedInputFailure>;
    explicit MonitoringQ4GovernedInputAuthority(MonitoringGovernedTrackedBundleCore&& value)
        : core_{std::move(value)} {}
    static auto authenticate_with(int, MonitoringCurrentRevisionTrackedCapture&&, Operations)
        -> std::expected<MonitoringQ4GovernedInputAuthority, MonitoringQ4GovernedInputFailure>;
    [[nodiscard]] auto prepare_candidate()
        -> std::expected<MonitoringQ4GovernedInputCandidate, MonitoringQ4GovernedInputFailure>;
    [[nodiscard]] auto finalize(MonitoringQ4GovernedInputCandidate&&) noexcept
        -> std::expected<MonitoringQ4GovernedInputValue, MonitoringQ4GovernedInputFailure>;
    [[nodiscard]] auto recheck() const
        -> std::expected<bool, MonitoringGovernedTrackedBundleFailure> {
        if (!core_)
            return std::unexpected{MonitoringGovernedTrackedBundleFailure{
                .kind = MonitoringGovernedTrackedBundleFailureKind::inactive}};
        return core_->recheck();
    }
    std::optional<MonitoringGovernedTrackedBundleCore> core_;
    bool consumed_{};
    bool candidate_prepared_{};
    friend class MonitoringQ4GovernedInputAuthorityTestAccess;
    friend class HostFrozenSourceAdmission;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4CompositionOwner;
};

} // namespace epycbench::monitoring::qualification::detail
