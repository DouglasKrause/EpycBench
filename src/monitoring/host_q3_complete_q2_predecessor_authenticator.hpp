#pragma once

#include "monitoring/host_q3_historical_git_authority.hpp"
#include "epycbench/core/digest.hpp"

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ3CompleteQ2PredecessorInput final {
    std::string qualification_id;
    std::string current_commit;
    std::string profile_bytes;
    std::string q1_plan_bytes;
    std::string q2_plan_bytes;
    std::string q3_plan_bytes;
    std::string host_environment_bytes;
    std::string q0b_evidence_bytes;
    std::string q1_evidence_bytes;
    std::string q2_evidence_bytes;
    std::string manifest_bytes;
    std::string checksums_bytes;
};

struct MonitoringQ3CompleteQ2PredecessorFailure final {
    core::Error primary;
    std::array<std::optional<core::Error>, 32> cleanup{};
    std::size_t cleanup_count{};
};

struct MonitoringQ3CompleteQ2PredecessorValue final {
    std::string q0_commit, q1_commit, q2_commit;
    std::string host_environment_digest, qualification_plan_digest;
    core::Sha256Digest q0b_evidence, q1_evidence, q2_evidence, manifest, checksums;
    bool q2_unsupported{};
};

class MonitoringQ1ValidatedPublicValues;
class MonitoringQ3CompleteQ2PredecessorAuthenticatorTestAccess;
class MonitoringQ3CompleteQ2PredecessorAuthority;
class MonitoringQ4CompleteQ3PredecessorAuthenticator;

class MonitoringQ3CompleteQ2PredecessorAuthenticator final {
  public:
    struct ExpectedDigests final {
        std::string host_environment, q0b, q1, q2, manifest, checksums;
    };
    struct Operations final {
        void* context{};
        std::expected<void, MonitoringQ3HistoricalGitFailure> (*authenticate_history)(
            void*, std::string_view, std::string_view, std::string_view, std::string_view,
            std::string_view, std::string_view, std::string_view, std::string_view){};
    };
    [[nodiscard]] static auto authenticate(MonitoringQ3CompleteQ2PredecessorInput, Operations)
        noexcept -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                                  MonitoringQ3CompleteQ2PredecessorFailure>;

  private:
    [[nodiscard]] static auto authenticate_with(MonitoringQ3CompleteQ2PredecessorInput, Operations,
                                                const ExpectedDigests&) noexcept
        -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                         MonitoringQ3CompleteQ2PredecessorFailure>;
    [[nodiscard]] static auto authenticate_campaign(MonitoringQ3CompleteQ2PredecessorInput,
        Operations, const MonitoringQ1ValidatedPublicValues&) noexcept
        -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                         MonitoringQ3CompleteQ2PredecessorFailure>;
    [[nodiscard]] static auto authenticate_impl(MonitoringQ3CompleteQ2PredecessorInput,
        Operations, const ExpectedDigests*, const MonitoringQ1ValidatedPublicValues*) noexcept
        -> std::expected<MonitoringQ3CompleteQ2PredecessorValue,
                         MonitoringQ3CompleteQ2PredecessorFailure>;
    friend class MonitoringQ3CompleteQ2PredecessorAuthenticatorTestAccess;
    friend class MonitoringQ3CompleteQ2PredecessorAuthority;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticator;
};

} // namespace epycbench::monitoring::qualification::detail
