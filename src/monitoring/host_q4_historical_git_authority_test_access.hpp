#pragma once
#include "monitoring/host_q4_historical_git_authority.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4HistoricalGitAuthorityTestAccess final {
  public:
    static auto authenticate_campaign(MonitoringQ4HistoricalGitAuthority::Operations operations,
        const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ4CompleteQ3PredecessorAuthority& snapshot) {
        return MonitoringQ4HistoricalGitAuthority::authenticate_campaign_with(
            operations, candidate, snapshot, MonitoringQ4HistoricalGitAuthority::bootstrap_failure());
    }
    using Operations = MonitoringQ4HistoricalGitAuthority::Operations;
    using BootstrapOperations = MonitoringQ4HistoricalGitAuthority::BootstrapOperations;
    static auto authenticate(Operations operations,
                             const MonitoringQ4GovernedInputCandidate& candidate) {
        return MonitoringQ4HistoricalGitAuthority::authenticate_with(
            operations, candidate, MonitoringQ4HistoricalGitAuthority::bootstrap_failure());
    }
    static auto authenticate_commits(Operations operations,
                                     const MonitoringQ4GovernedInputCandidate& candidate,
                                     const std::array<std::string_view, 4>& commits) {
        return MonitoringQ4HistoricalGitAuthority::authenticate_commits_with(
            operations, candidate, commits,
            MonitoringQ4HistoricalGitAuthority::bootstrap_failure());
    }
    static auto authenticate_delegated(
        Operations operations, const MonitoringQ4GovernedInputCandidate& candidate,
        MonitoringQ4HistoricalGitFailure fallback, BootstrapOperations bootstrap) {
        return MonitoringQ4HistoricalGitAuthority::authenticate_delegated(
            operations, candidate, std::move(fallback), bootstrap);
    }
    static auto bootstrap_failure() {
        return MonitoringQ4HistoricalGitAuthority::bootstrap_failure();
    }
    static auto binding(const MonitoringQ4HistoricalGitValue& value) noexcept {
        return value.binding_;
    }
    static auto matches(const MonitoringQ4HistoricalGitValue& value,
                        const MonitoringQ4GovernedInputCandidate& candidate) noexcept {
        return value.binding_ == candidate.binding_;
    }
    static auto make_value(std::string current, std::shared_ptr<const void> binding) {
        return MonitoringQ4HistoricalGitValue{
            {"4721bcf2efe66dfcfd9532fde4853480937d7fed",
             "5990de3e0cfc80fbd74120125d9d2ebcfecbf72e",
             "fda00b486bd997210f35d513afc06bd29191490d",
             "485d5c314b6c32ab1b2a7d17f095b8325c387cd1", std::move(current)},
            std::move(binding)};
    }
    static void replace_current(MonitoringQ4HistoricalGitValue& value,
                                std::string current) {
        value.commits_[4] = std::move(current);
    }
};
} // namespace epycbench::monitoring::qualification::detail
