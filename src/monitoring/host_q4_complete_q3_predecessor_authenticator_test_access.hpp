#pragma once
#include "monitoring/host_q4_complete_q3_predecessor_authenticator.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CompleteQ3PredecessorAuthenticatorTestAccess final {
  public:
    static auto authenticate_campaign(MonitoringQ4CompleteQ3PredecessorInput input,
        const MonitoringQ4GovernedInputCandidate& candidate,
        const MonitoringQ4HistoricalGitValue& history,
        const MonitoringQ1ValidatedPublicValues& values) {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_campaign(
            std::move(input), candidate, history, values);
    }
    using ExpectedDigests = MonitoringQ4CompleteQ3PredecessorAuthenticator::ExpectedDigests;
    using CanonicalArtifactRole =
        MonitoringQ4CompleteQ3PredecessorAuthenticator::CanonicalArtifactRole;
    static auto authenticate(MonitoringQ4CompleteQ3PredecessorInput input,
                             const MonitoringQ4GovernedInputCandidate& candidate,
                             const MonitoringQ4HistoricalGitValue& history) {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate(
            std::move(input), candidate, history);
    }
    static auto authenticate(MonitoringQ4CompleteQ3PredecessorInput input,
                             const MonitoringQ4GovernedInputCandidate& candidate,
                             const MonitoringQ4HistoricalGitValue& history,
                             const ExpectedDigests& expected) {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::authenticate_with(
            std::move(input), candidate, history, expected);
    }
    static auto artifacts(const MonitoringQ4CompleteQ3PredecessorValue& value) noexcept
        -> const MonitoringQ4CompleteQ3PredecessorInput& { return value.artifacts_; }
    static auto digests(const MonitoringQ4CompleteQ3PredecessorValue& value) noexcept
        -> const std::array<core::Sha256Digest, 7>& { return value.digests_; }
    static auto validate_environment(std::string_view bytes) noexcept {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_environment(bytes);
    }
    static auto validate_outer_canonical(CanonicalArtifactRole role,
                                         std::string_view bytes) noexcept {
        return MonitoringQ4CompleteQ3PredecessorAuthenticator::validate_outer_canonical(
            role, bytes);
    }
};
} // namespace epycbench::monitoring::qualification::detail
