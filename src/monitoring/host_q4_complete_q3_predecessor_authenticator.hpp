#pragma once

#include "monitoring/host_q4_historical_git_authority.hpp"
#include "monitoring/host_q3_complete_q2_predecessor_authenticator.hpp"
#include "epycbench/topology/fingerprint.hpp"

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4CompleteQ3PredecessorAuthority;

struct MonitoringQ4CompleteQ3PredecessorInput final {
    std::string host_environment;
    std::string q0b_evidence;
    std::string q1_evidence;
    std::string q2_evidence;
    std::string q3_evidence;
    std::string manifest;
    std::string checksums;
};

struct MonitoringQ4AdmittedLinuxCpuId final {
    std::uint32_t value{};
    auto operator<=>(const MonitoringQ4AdmittedLinuxCpuId&) const = default;
};
struct MonitoringQ4BandwidthWorkerCount final {
    std::uint64_t value{};
    auto operator<=>(const MonitoringQ4BandwidthWorkerCount&) const = default;
};
struct MonitoringQ4BasePageSizeBytes final {
    std::uint64_t value{};
    auto operator<=>(const MonitoringQ4BasePageSizeBytes&) const = default;
};
struct MonitoringQ4AuthenticatedHostProjection final {
    std::string host_environment_digest;
    std::string qualification_plan_digest;
    std::string physical_fingerprint;
    std::string effective_fingerprint;
    MonitoringQ4BasePageSizeBytes page_size_bytes{};
    MonitoringQ4BandwidthWorkerCount bandwidth_worker_count{};
    std::vector<MonitoringQ4AdmittedLinuxCpuId> admitted_linux_cpus;
    std::optional<core::Sha256Digest> authenticated_host_environment_digest;
    std::optional<core::Sha256Digest> authenticated_qualification_plan_digest;
    std::optional<topology::PhysicalTopologyFingerprint> authenticated_physical_fingerprint;
    std::optional<topology::EffectiveTopologyFingerprint> authenticated_effective_fingerprint;
    [[nodiscard]] auto admitted_cpu_count() const noexcept -> std::uint64_t {
        return admitted_linux_cpus.size();
    }
};

class MonitoringQ4CompleteQ3PredecessorValue final {
  public:
    MonitoringQ4CompleteQ3PredecessorValue(
        const MonitoringQ4CompleteQ3PredecessorValue&) = delete;
    MonitoringQ4CompleteQ3PredecessorValue(
        MonitoringQ4CompleteQ3PredecessorValue&&) noexcept = default;
    auto operator=(const MonitoringQ4CompleteQ3PredecessorValue&)
        -> MonitoringQ4CompleteQ3PredecessorValue& = delete;
    auto operator=(MonitoringQ4CompleteQ3PredecessorValue&&)
        -> MonitoringQ4CompleteQ3PredecessorValue& = delete;

    [[nodiscard]] auto projection() const noexcept
        -> const MonitoringQ4AuthenticatedHostProjection& { return projection_; }
    [[nodiscard]] auto q3_build_identity() const noexcept -> std::string_view {
        return q3_build_identity_;
    }
    [[nodiscard]] auto qualification_id() const noexcept -> std::string_view { return qid_; }
    [[nodiscard]] auto commits() const noexcept -> const std::array<std::string, 5>& {
        return commits_;
    }
    [[nodiscard]] auto digests() const noexcept -> const std::array<core::Sha256Digest, 7>& {
        return digests_;
    }
    [[nodiscard]] auto q2_unsupported() const noexcept -> bool { return q2_unsupported_; }
    [[nodiscard]] auto q3_counts() const noexcept -> const std::array<std::uint64_t, 3>& {
        return q3_counts_;
    }
    [[nodiscard]] auto q3_resources_retired() const noexcept -> bool {
        return q3_resources_retired_;
    }

  private:
    MonitoringQ4CompleteQ3PredecessorValue(
        MonitoringQ4CompleteQ3PredecessorInput artifacts,
        std::array<core::Sha256Digest, 7> digests,
        MonitoringQ4AuthenticatedHostProjection projection,
        std::string build_identity, std::array<std::string, 5> commits,
        bool q2_unsupported, std::array<std::uint64_t, 3> q3_counts,
        bool q3_resources_retired, std::shared_ptr<const void> binding,
        std::string qualification_id = "MON-007.host-v2.20260906-05") noexcept
        : artifacts_{std::move(artifacts)}, digests_{std::move(digests)},
          projection_{std::move(projection)}, q3_build_identity_{std::move(build_identity)},
          qid_{std::move(qualification_id)}, commits_{std::move(commits)},
          q2_unsupported_{q2_unsupported}, q3_counts_{q3_counts},
          q3_resources_retired_{q3_resources_retired}, binding_{std::move(binding)} {}
    MonitoringQ4CompleteQ3PredecessorInput artifacts_;
    std::array<core::Sha256Digest, 7> digests_;
    MonitoringQ4AuthenticatedHostProjection projection_;
    std::string q3_build_identity_;
    std::string qid_;
    std::array<std::string, 5> commits_;
    bool q2_unsupported_{};
    std::array<std::uint64_t, 3> q3_counts_{};
    bool q3_resources_retired_{};
    std::shared_ptr<const void> binding_;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticator;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticatorTestAccess;
    friend class MonitoringQ4CompleteQ3PredecessorAuthority;
    friend class HostFrozenSourceAdmission;
    friend class HostFrozenSourceAdmissionTestAccess;
};

struct MonitoringQ4CompleteQ3PredecessorFailure final { core::Error primary; };

class MonitoringQ4CompleteQ3PredecessorAuthenticator final {
  private:
    struct CompletedPrefix final {
        std::array<std::string, 2> manifest_checksums;
        std::string producer_commit;
    };
    static auto completed_prefix(const std::array<std::string, 7>&,
                                 std::string_view q4_bytes,
                                 const MonitoringQ4GovernedInputCandidate&,
                                 const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<CompletedPrefix, MonitoringQ4CompleteQ3PredecessorFailure>;
    enum class CanonicalArtifactRole : unsigned char {
        host_environment, q1_evidence, q2_evidence, q3_evidence
    };
    struct ExpectedDigests final {
        std::array<std::string, 7> artifacts;
        MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests complete_q2;
        void* fault_context{};
        void (*fault)(void*, unsigned int){};
    };
    static auto authenticate(MonitoringQ4CompleteQ3PredecessorInput,
                             const MonitoringQ4GovernedInputCandidate&,
                             const MonitoringQ4HistoricalGitValue&)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                         MonitoringQ4CompleteQ3PredecessorFailure>;
    static auto authenticate_with(MonitoringQ4CompleteQ3PredecessorInput,
                                  const MonitoringQ4GovernedInputCandidate&,
                                  const MonitoringQ4HistoricalGitValue&,
                                  const ExpectedDigests&)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                         MonitoringQ4CompleteQ3PredecessorFailure>;
    static auto authenticate_campaign(MonitoringQ4CompleteQ3PredecessorInput,
        const MonitoringQ4GovernedInputCandidate&, const MonitoringQ4HistoricalGitValue&,
        const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                         MonitoringQ4CompleteQ3PredecessorFailure>;
    static auto authenticate_impl(MonitoringQ4CompleteQ3PredecessorInput,
        const MonitoringQ4GovernedInputCandidate&, const MonitoringQ4HistoricalGitValue&,
        const ExpectedDigests*, const MonitoringQ1ValidatedPublicValues*)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                         MonitoringQ4CompleteQ3PredecessorFailure>;
    static auto candidate_active(const MonitoringQ4GovernedInputCandidate&) noexcept -> bool;
    static auto retained_depth_valid(std::string_view, std::size_t maximum) noexcept -> bool;
    static void set_prefix_test_hook(void (*)(unsigned char) noexcept) noexcept;
    static auto validate_environment(std::string_view) noexcept -> bool;
    static auto validate_outer_canonical(CanonicalArtifactRole, std::string_view) noexcept -> bool;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticatorTestAccess;
    friend class MonitoringQ4CompleteQ3PredecessorAuthority;
    friend class HostFrozenSourceAdmission;
    friend class HostFrozenSourceAdmissionTestAccess;
    friend class MonitoringQ4RetainedEvidence;
};

} // namespace epycbench::monitoring::qualification::detail
