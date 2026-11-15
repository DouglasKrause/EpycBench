#pragma once

#include "epycbench/runtime/ipc/payloads.hpp"
#include "epycbench/runtime/statistics_production_policy.hpp"
#include "runtime/pairwise_monitoring_host_capacity_admission.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::runtime::detail {

struct PairwiseHostCapacityRequestTranscriptConstruction;
struct PairwiseV2PhaseAProtocolAccess;
struct PairwiseV2ContextualAdmissionAccess;

inline constexpr std::string_view pairwise_host_capacity_startup_protocol =
    "epycbench.monitoring-host-capacity-startup/v1";
inline constexpr std::string_view pairwise_host_capacity_offer_profile =
    "epycbench.monitoring-host-capacity-offer/v1";
inline constexpr std::string_view pairwise_host_capacity_accepted_profile =
    "epycbench.monitoring-host-capacity-accepted/v1";
inline constexpr std::string_view pairwise_host_capacity_rejected_profile =
    "epycbench.monitoring-host-capacity-rejected/v1";
inline constexpr std::string_view pairwise_v2_protocol =
    "epycbench.controller-runner-ipc/v2";
inline constexpr std::string_view pairwise_request_profile =
    "epycbench.pairwise-request/v1";

enum class PairwiseHostCapacityStartupFailure : unsigned char {
    invalid_argument,
    invalid_encoding,
    invalid_envelope,
    unsupported_protocol,
    unexpected_message,
    sequence_mismatch,
    request_binding_mismatch,
    lower_authority_mismatch,
    capacity_contract_identity_mismatch,
    finalized_error_profile_identity_mismatch,
    pairwise_policy_mismatch,
    acknowledgement_already_issued,
    frame_zero,
    frame_over_limit,
    truncated_frame,
    io_failure,
    allocation,
    contained_exception,
    contained_unknown_failure
};

enum class PairwiseHostCapacityRejectionReason : unsigned char {
    runner_authority_unavailable,
    request_binding_mismatch,
    lower_schema_or_algorithm_mismatch,
    lower_authority_mismatch,
    provenance_mismatch,
    topology_mismatch,
    present_cpu_mismatch,
    capacity_contract_identity_mismatch,
    finalized_error_profile_identity_mismatch,
    pairwise_policy_mismatch,
    allocation,
    contained_exception,
    contained_unknown_failure
};

enum class PairwiseInitialProtocol : unsigned char { legacy_v1, host_capacity_startup };

class PairwiseHostCapacityRequestTranscript final {
  public:
    ~PairwiseHostCapacityRequestTranscript();
    PairwiseHostCapacityRequestTranscript(const PairwiseHostCapacityRequestTranscript&) = delete;
    PairwiseHostCapacityRequestTranscript(PairwiseHostCapacityRequestTranscript&&) noexcept =
        default;
    auto operator=(const PairwiseHostCapacityRequestTranscript&)
        -> PairwiseHostCapacityRequestTranscript& = delete;
    auto operator=(PairwiseHostCapacityRequestTranscript&&)
        -> PairwiseHostCapacityRequestTranscript& = delete;

    [[nodiscard]] auto first_request_id() const noexcept -> const ipc::RequestId& {
        return first_id_;
    }
    [[nodiscard]] auto second_request_id() const noexcept -> const ipc::RequestId& {
        return second_id_;
    }
    [[nodiscard]] auto canonical_body() const noexcept -> std::string_view { return body_; }
    [[nodiscard]] auto body_sha256() const noexcept -> const core::Sha256Digest& {
        return body_sha256_;
    }
    [[nodiscard]] auto first() const noexcept -> const ipc::PlacementRequest& { return first_; }
    [[nodiscard]] auto second() const noexcept -> const ipc::PlacementRequest& { return second_; }
    [[nodiscard]] auto resolved_comparison_order_seed() const noexcept -> std::uint64_t {
        return seed_;
    }
    [[nodiscard]] auto statistics_source() const noexcept
        -> const StatisticsComparisonPolicySource& { return statistics_; }

  private:
    PairwiseHostCapacityRequestTranscript(
        ipc::RequestId first_id, ipc::RequestId second_id, std::string body,
        core::Sha256Digest body_sha256, ipc::PlacementRequest first,
        ipc::PlacementRequest second, std::uint64_t seed,
        StatisticsComparisonPolicySource statistics) noexcept
        : first_id_{first_id}, second_id_{second_id}, body_{std::move(body)},
          body_sha256_{body_sha256}, first_{std::move(first)}, second_{std::move(second)},
          seed_{seed}, statistics_{std::move(statistics)} {}

    ipc::RequestId first_id_;
    ipc::RequestId second_id_;
    std::string body_;
    core::Sha256Digest body_sha256_;
    ipc::PlacementRequest first_;
    ipc::PlacementRequest second_;
    std::uint64_t seed_{};
    StatisticsComparisonPolicySource statistics_;

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseHostCapacityRequestTranscriptConstruction;
};

class PreparedPairwiseHostCapacityOffer final {
  public:
    PreparedPairwiseHostCapacityOffer(const PreparedPairwiseHostCapacityOffer&) = delete;
    PreparedPairwiseHostCapacityOffer(PreparedPairwiseHostCapacityOffer&&) noexcept;
    auto operator=(const PreparedPairwiseHostCapacityOffer&)
        -> PreparedPairwiseHostCapacityOffer& = delete;
    auto operator=(PreparedPairwiseHostCapacityOffer&&)
        -> PreparedPairwiseHostCapacityOffer& = delete;

  private:
    PreparedPairwiseHostCapacityOffer(
        std::string body, core::Sha256Digest body_sha256, ipc::RequestId first,
        ipc::RequestId second, core::Sha256Digest request_sha256,
        core::Sha256Digest lower_identity, core::Sha256Digest capacity_identity,
        core::Sha256Digest profile_identity) noexcept
        : body_{std::move(body)}, body_sha256_{body_sha256}, first_{first}, second_{second},
          request_sha256_{request_sha256}, lower_identity_{lower_identity},
          capacity_identity_{capacity_identity}, profile_identity_{profile_identity},
          active_{true} {}

    std::string body_;
    core::Sha256Digest body_sha256_;
    ipc::RequestId first_;
    ipc::RequestId second_;
    core::Sha256Digest request_sha256_;
    core::Sha256Digest lower_identity_;
    core::Sha256Digest capacity_identity_;
    core::Sha256Digest profile_identity_;
    bool active_{};

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2RunnerSessionAccess;
};

class PairwiseHostCapacityOfferValidationState final {
  public:
    [[nodiscard]] auto body_sha256() const noexcept -> const core::Sha256Digest& {
        return body_sha256_;
    }

  private:
    PairwiseHostCapacityOfferValidationState(
        core::Sha256Digest body, ipc::RequestId first, ipc::RequestId second,
        core::Sha256Digest request, core::Sha256Digest lower,
        core::Sha256Digest capacity, core::Sha256Digest profile) noexcept
        : body_sha256_{body}, first_{first}, second_{second}, request_sha256_{request},
          lower_identity_{lower}, capacity_identity_{capacity}, profile_identity_{profile} {}

    core::Sha256Digest body_sha256_;
    ipc::RequestId first_;
    ipc::RequestId second_;
    core::Sha256Digest request_sha256_;
    core::Sha256Digest lower_identity_;
    core::Sha256Digest capacity_identity_;
    core::Sha256Digest profile_identity_;

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2PhaseAProtocolAccess;
};

class AuthoredPairwiseHostCapacityOffer final {
  public:
    AuthoredPairwiseHostCapacityOffer(const AuthoredPairwiseHostCapacityOffer&) = delete;
    AuthoredPairwiseHostCapacityOffer(AuthoredPairwiseHostCapacityOffer&&) noexcept = default;
    auto operator=(const AuthoredPairwiseHostCapacityOffer&)
        -> AuthoredPairwiseHostCapacityOffer& = delete;
    auto operator=(AuthoredPairwiseHostCapacityOffer&&)
        -> AuthoredPairwiseHostCapacityOffer& = delete;

    [[nodiscard]] auto canonical_body() const noexcept -> std::string_view { return body_; }
    [[nodiscard]] auto validation() const noexcept
        -> const PairwiseHostCapacityOfferValidationState& { return validation_; }

  private:
    AuthoredPairwiseHostCapacityOffer(
        std::string body, PairwiseHostCapacityOfferValidationState validation) noexcept
        : body_{std::move(body)}, validation_{std::move(validation)} {}

    std::string body_;
    PairwiseHostCapacityOfferValidationState validation_;

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2PhaseAProtocolAccess;
};

class TransportedLinuxMonitoringCapacityCommitment final {
  public:
    TransportedLinuxMonitoringCapacityCommitment(
        const TransportedLinuxMonitoringCapacityCommitment&) = delete;
    TransportedLinuxMonitoringCapacityCommitment(
        TransportedLinuxMonitoringCapacityCommitment&&) noexcept = default;
    auto operator=(const TransportedLinuxMonitoringCapacityCommitment&)
        -> TransportedLinuxMonitoringCapacityCommitment& = delete;
    auto operator=(TransportedLinuxMonitoringCapacityCommitment&&)
        -> TransportedLinuxMonitoringCapacityCommitment& = delete;

    [[nodiscard]] auto first_request_id() const noexcept -> const ipc::RequestId& {
        return first_;
    }
    [[nodiscard]] auto second_request_id() const noexcept -> const ipc::RequestId& {
        return second_;
    }
    [[nodiscard]] auto offer_body_sha256() const noexcept -> const core::Sha256Digest& {
        return offer_sha256_;
    }
    [[nodiscard]] auto request_body_sha256() const noexcept -> const core::Sha256Digest& {
        return request_sha256_;
    }

  private:
    TransportedLinuxMonitoringCapacityCommitment(
        ipc::RequestId first, ipc::RequestId second, core::ByteCount page,
        std::uint32_t kernel_maximum, std::vector<core::LinuxCpuId> present,
        topology::PhysicalTopologyFingerprint physical,
        topology::EffectiveTopologyFingerprint effective,
        platform_linux::detail::LinuxMonitoringCapacityProvenanceKind provenance_kind,
        core::Sha256Digest provenance, core::Sha256Digest lower_identity,
        core::Sha256Digest capacity_identity, core::Sha256Digest profile_identity,
        core::Sha256Digest request_sha256, core::Sha256Digest offer_sha256,
        bool lower_contract_matches) noexcept
        : first_{first}, second_{second}, page_{page}, kernel_maximum_{kernel_maximum},
          present_{std::move(present)}, physical_{std::move(physical)},
          effective_{std::move(effective)}, provenance_kind_{provenance_kind},
          provenance_{provenance}, lower_identity_{lower_identity},
          capacity_identity_{capacity_identity}, profile_identity_{profile_identity},
          request_sha256_{request_sha256}, offer_sha256_{offer_sha256},
          lower_contract_matches_{lower_contract_matches} {}

    ipc::RequestId first_;
    ipc::RequestId second_;
    core::ByteCount page_;
    std::uint32_t kernel_maximum_{};
    std::vector<core::LinuxCpuId> present_;
    topology::PhysicalTopologyFingerprint physical_;
    topology::EffectiveTopologyFingerprint effective_;
    platform_linux::detail::LinuxMonitoringCapacityProvenanceKind provenance_kind_{};
    core::Sha256Digest provenance_;
    core::Sha256Digest lower_identity_;
    core::Sha256Digest capacity_identity_;
    core::Sha256Digest profile_identity_;
    core::Sha256Digest request_sha256_;
    core::Sha256Digest offer_sha256_;
    bool lower_contract_matches_{};

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
};

class ValidatedRemoteCapacityCommitment final {
  public:
    ValidatedRemoteCapacityCommitment(const ValidatedRemoteCapacityCommitment&) = delete;
    ValidatedRemoteCapacityCommitment(ValidatedRemoteCapacityCommitment&& other) noexcept;
    auto operator=(const ValidatedRemoteCapacityCommitment&)
        -> ValidatedRemoteCapacityCommitment& = delete;
    auto operator=(ValidatedRemoteCapacityCommitment&&)
        -> ValidatedRemoteCapacityCommitment& = delete;

  private:
    enum class ProjectionConsumption : unsigned char {
        accepted,
        mismatch,
        allocation,
        contained_exception,
        contained_unknown_failure
    };
    ValidatedRemoteCapacityCommitment(
        ipc::RequestId first, ipc::RequestId second, core::Sha256Digest request,
        core::Sha256Digest offer, core::Sha256Digest capacity,
        core::Sha256Digest profile, core::Sha256Digest first_wire,
        core::Sha256Digest second_wire, std::uint64_t seed,
        StatisticsComparisonPolicySource statistics,
        platform_linux::detail::LinuxMonitoringCapacityLocalBindingSeal binding) noexcept
        : first_{first}, second_{second}, request_{request}, offer_{offer},
          capacity_{capacity}, profile_{profile}, first_wire_{first_wire},
          second_wire_{second_wire}, seed_{seed}, statistics_{std::move(statistics)},
          binding_{std::move(binding)}, active_{true} {}
    [[nodiscard]] auto consume_for_projection(
        const AdmittedPairwiseProductionInputs&) noexcept -> ProjectionConsumption;

    ipc::RequestId first_;
    ipc::RequestId second_;
    core::Sha256Digest request_;
    core::Sha256Digest offer_;
    core::Sha256Digest capacity_;
    core::Sha256Digest profile_;
    core::Sha256Digest first_wire_;
    core::Sha256Digest second_wire_;
    std::uint64_t seed_{};
    StatisticsComparisonPolicySource statistics_;
    platform_linux::detail::LinuxMonitoringCapacityLocalBindingSeal binding_;
    bool active_{};

    friend struct PairwiseMonitoringHostCapacityAdmissionAccess;
    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2ContextualAdmissionAccess;
};

class HostCapacityAcceptedAckCapability final {
  public:
    HostCapacityAcceptedAckCapability(const HostCapacityAcceptedAckCapability&) = delete;
    HostCapacityAcceptedAckCapability(HostCapacityAcceptedAckCapability&& other) noexcept;
    auto operator=(const HostCapacityAcceptedAckCapability&)
        -> HostCapacityAcceptedAckCapability& = delete;
    auto operator=(HostCapacityAcceptedAckCapability&&)
        -> HostCapacityAcceptedAckCapability& = delete;

  private:
    HostCapacityAcceptedAckCapability(
        ipc::RequestId first, ipc::RequestId second, core::Sha256Digest request,
        core::Sha256Digest offer, core::Sha256Digest lower,
        core::Sha256Digest capacity, core::Sha256Digest profile) noexcept
        : first_{first}, second_{second}, request_{request}, offer_{offer}, lower_{lower},
          capacity_{capacity}, profile_{profile}, active_{true} {}

    ipc::RequestId first_;
    ipc::RequestId second_;
    core::Sha256Digest request_;
    core::Sha256Digest offer_;
    core::Sha256Digest lower_;
    core::Sha256Digest capacity_;
    core::Sha256Digest profile_;
    bool active_{};

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2PhaseAProtocolAccess;
};

class HostCapacityRejectedAckCapability final {
  public:
    HostCapacityRejectedAckCapability(const HostCapacityRejectedAckCapability&) = delete;
    HostCapacityRejectedAckCapability(HostCapacityRejectedAckCapability&& other) noexcept;
    auto operator=(const HostCapacityRejectedAckCapability&)
        -> HostCapacityRejectedAckCapability& = delete;
    auto operator=(HostCapacityRejectedAckCapability&&)
        -> HostCapacityRejectedAckCapability& = delete;

  private:
    HostCapacityRejectedAckCapability(
        ipc::RequestId first, ipc::RequestId second, core::Sha256Digest request,
        core::Sha256Digest offer, PairwiseHostCapacityRejectionReason reason) noexcept
        : first_{first}, second_{second}, request_{request}, offer_{offer}, reason_{reason},
          active_{true} {}
    ipc::RequestId first_;
    ipc::RequestId second_;
    core::Sha256Digest request_;
    core::Sha256Digest offer_;
    PairwiseHostCapacityRejectionReason reason_{};
    bool active_{};
    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2ContextualAdmissionAccess;
};

class AdmittedPairwiseHostCapacityStartup final {
  public:
    AdmittedPairwiseHostCapacityStartup(const AdmittedPairwiseHostCapacityStartup&) = delete;
    AdmittedPairwiseHostCapacityStartup(AdmittedPairwiseHostCapacityStartup&&) noexcept = default;
    auto operator=(const AdmittedPairwiseHostCapacityStartup&)
        -> AdmittedPairwiseHostCapacityStartup& = delete;
    auto operator=(AdmittedPairwiseHostCapacityStartup&&)
        -> AdmittedPairwiseHostCapacityStartup& = delete;

    [[nodiscard]] auto admission() const noexcept
        -> const PairwiseMonitoringHostCapacityAdmission& { return admission_; }

  private:
    AdmittedPairwiseHostCapacityStartup(
        PairwiseMonitoringHostCapacityAdmission admission, ipc::RequestId first,
        ipc::RequestId second, core::Sha256Digest request, core::Sha256Digest offer,
        core::Sha256Digest capacity, core::Sha256Digest profile,
        AdmittedPlacementIdentity first_identity,
        AdmittedPlacementIdentity second_identity) noexcept
        : admission_{std::move(admission)}, first_{first}, second_{second}, request_{request},
          offer_{offer}, capacity_{capacity}, profile_{profile},
          first_identity_{std::move(first_identity)},
          second_identity_{std::move(second_identity)} {}

    PairwiseMonitoringHostCapacityAdmission admission_;
    ipc::RequestId first_;
    ipc::RequestId second_;
    core::Sha256Digest request_;
    core::Sha256Digest offer_;
    core::Sha256Digest capacity_;
    core::Sha256Digest profile_;
    AdmittedPlacementIdentity first_identity_;
    AdmittedPlacementIdentity second_identity_;
    bool ack_issued_{};

    friend struct PairwiseMonitoringHostCapacityStartupAccess;
    friend struct PairwiseV2RunnerSessionAccess;
};

struct ParsedHostCapacityAcceptedAck final {
    ipc::RequestId first;
    ipc::RequestId second;
    core::Sha256Digest request;
    core::Sha256Digest offer;
    core::Sha256Digest lower;
    core::Sha256Digest capacity;
    core::Sha256Digest profile;
};

struct ParsedHostCapacityRejectedAck final {
    ipc::RequestId first;
    ipc::RequestId second;
    core::Sha256Digest request;
    core::Sha256Digest offer;
    PairwiseHostCapacityRejectionReason reason;
};

struct PairwiseMonitoringHostCapacityStartupAccess final {
    using LowerAuthority =
        platform_linux::detail::AuthenticatedLinuxMonitoringCapacityAuthority;
    using LowerOperations = LowerAuthority::Operations;

    [[nodiscard]] static auto prepare_request(
        ipc::RequestId first_id, ipc::RequestId second_id,
        const ipc::PlacementRequest& first, const ipc::PlacementRequest& second,
        std::uint64_t resolved_comparison_order_seed,
        const StatisticsComparisonPolicySource& statistics) noexcept
        -> std::expected<PairwiseHostCapacityRequestTranscript,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto parse_request(std::string_view canonical_body) noexcept
        -> std::expected<PairwiseHostCapacityRequestTranscript,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto prepare_offer(
        const PairwiseHostCapacityRequestTranscript&, const LowerAuthority& controller) noexcept
        -> std::expected<PreparedPairwiseHostCapacityOffer,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto author_offer(PreparedPairwiseHostCapacityOffer) noexcept
        -> std::expected<AuthoredPairwiseHostCapacityOffer,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto parse_offer(std::string_view canonical_body) noexcept
        -> std::expected<TransportedLinuxMonitoringCapacityCommitment,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto mint_controller(
        const PairwiseHostCapacityRequestTranscript&, const RunnerContext&) noexcept
        -> std::expected<LowerAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto mint_controller_with(
        const PairwiseHostCapacityRequestTranscript&, const RunnerContext&,
        LowerOperations) noexcept
        -> std::expected<LowerAuthority,
                         platform_linux::detail::LinuxMonitoringCapacityFailure>;
    [[nodiscard]] static auto mint_runner(
        const TransportedLinuxMonitoringCapacityCommitment&,
        const PairwiseHostCapacityRequestTranscript&, const RunnerContext&) noexcept
        -> std::expected<LowerAuthority, HostCapacityRejectedAckCapability>;
    [[nodiscard]] static auto mint_runner_with(
        const TransportedLinuxMonitoringCapacityCommitment&,
        const PairwiseHostCapacityRequestTranscript&, const RunnerContext&,
        LowerOperations) noexcept
        -> std::expected<LowerAuthority, HostCapacityRejectedAckCapability>;
    [[nodiscard]] static auto gate_a(
        const TransportedLinuxMonitoringCapacityCommitment&,
        const PairwiseHostCapacityRequestTranscript&, const LowerAuthority&) noexcept
        -> std::expected<ValidatedRemoteCapacityCommitment,
                         HostCapacityRejectedAckCapability>;
    [[nodiscard]] static auto gate_b(
        const AdmittedPairwiseProductionInputs&, ValidatedRemoteCapacityCommitment proof,
        LowerAuthority runner) noexcept
        -> std::expected<AdmittedPairwiseHostCapacityStartup,
                         HostCapacityRejectedAckCapability>;
    [[nodiscard]] static auto issue_accepted_ack(
        AdmittedPairwiseHostCapacityStartup&) noexcept
        -> std::expected<HostCapacityAcceptedAckCapability,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto encode_accepted_ack(
        HostCapacityAcceptedAckCapability capability) noexcept
        -> std::expected<std::string, PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto parse_accepted_ack(std::string_view) noexcept
        -> std::expected<ParsedHostCapacityAcceptedAck,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto validate_accepted_ack(
        const PairwiseHostCapacityOfferValidationState&, const LowerAuthority& controller,
        const ParsedHostCapacityAcceptedAck&) noexcept
        -> std::expected<void, PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto encode_rejected_ack(
        HostCapacityRejectedAckCapability) noexcept
        -> std::expected<std::string, PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto parse_rejected_ack(std::string_view) noexcept
        -> std::expected<ParsedHostCapacityRejectedAck,
                         PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto dispatch_initial_protocol(std::string_view canonical_body) noexcept
        -> std::expected<PairwiseInitialProtocol, PairwiseHostCapacityStartupFailure>;

    [[nodiscard]] static auto encode_frame(std::string_view canonical_body) noexcept
        -> std::expected<std::vector<std::byte>, PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto read_frame(ipc::ByteReader&) noexcept
        -> std::expected<std::optional<std::string>, PairwiseHostCapacityStartupFailure>;
    [[nodiscard]] static auto write_frame(ipc::ByteWriter&,
                                          std::string_view canonical_body) noexcept
        -> std::expected<void, PairwiseHostCapacityStartupFailure>;
};

} // namespace epycbench::runtime::detail
