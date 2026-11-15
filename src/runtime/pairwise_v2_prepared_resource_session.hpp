#pragma once

#include "results/prepared_pairwise_run_result.hpp"
#include "runtime/finalized_evidence_prepared_direct_codec.hpp"
#include "runtime/finalized_evidence_prepared_resources.hpp"
#include "runtime/finalized_evidence_transport.hpp"
#include "runtime/stage_b_resource_policy.hpp"

#include <expected>
#include <memory>
#include <string_view>

namespace epycbench::runtime::detail {

enum class PairwiseV2PreparedSessionFailure : std::uint8_t {
    invalid_state,
    capacity_offer_mismatch,
    capacity_ack_mismatch,
    request_binding_mismatch,
    prepared_offer_mismatch,
    prepared_ack_mismatch,
    allocation_failure,
    arithmetic_overflow,
    resource_policy,
};

struct PreparedEvidenceSessionIdentity final {
    wire_v1::detail::StartupSessionId startup_session;
    FinalizedEvidenceTransportIdentity pair;
    core::Sha256Digest request_body_sha256;
    core::Sha256Digest capacity_offer_body_sha256;
    core::Sha256Digest capacity_body_sha256;
    core::Sha256Digest capacity_ack_body_sha256;
    core::Sha256Digest prepared_offer_body_sha256;
    core::Sha256Digest prepared_resource_sha256;
    core::Sha256Digest prepared_ack_body_sha256;
    core::Sha256Digest wire_registry_sha256;
    core::Sha256Digest source_manifest_sha256;

    friend auto operator==(const PreparedEvidenceSessionIdentity&,
                           const PreparedEvidenceSessionIdentity&) noexcept -> bool = default;
};

class PreparedRunnerEvidenceSession;
class PreparedControllerEvidenceSession;

// A source-authenticated, role-local resource binding. Creation is the last
// allocating operation. A typed take is one-use and moves all prepared
// resources to the role owner without allocation.
class PreparedEvidenceSessionBinding final {
  public:
    PreparedEvidenceSessionBinding(const PreparedEvidenceSessionBinding&) = delete;
    PreparedEvidenceSessionBinding(PreparedEvidenceSessionBinding&&) noexcept;
    auto operator=(const PreparedEvidenceSessionBinding&)
        -> PreparedEvidenceSessionBinding& = delete;
    auto operator=(PreparedEvidenceSessionBinding&&) -> PreparedEvidenceSessionBinding& = delete;

    [[nodiscard]] auto identity() const noexcept -> const PreparedEvidenceSessionIdentity& {
        return identity_;
    }
    ~PreparedEvidenceSessionBinding() noexcept;
    [[nodiscard]] auto resource() const noexcept
        -> const prepared_resources_v1::PreparedResourceValue*;
    [[nodiscard]] auto transport_plan() const noexcept -> FinalizedEvidenceTransportPlan {
        return plan_;
    }
    [[nodiscard]] auto take_runner() noexcept
        -> std::expected<PreparedRunnerEvidenceSession, PairwiseV2PreparedSessionFailure>;
    [[nodiscard]] auto take_controller() noexcept
        -> std::expected<PreparedControllerEvidenceSession, PairwiseV2PreparedSessionFailure>;

  private:
    struct Storage;
    PreparedEvidenceSessionBinding(PreparedEvidenceSessionIdentity identity,
                                   std::unique_ptr<Storage> storage,
                                   FinalizedEvidenceTransportPlan plan) noexcept;

    PreparedEvidenceSessionIdentity identity_;
    std::unique_ptr<Storage> storage_;
    FinalizedEvidenceTransportPlan plan_;

    friend class PairwiseV2PreparedControllerSession;
    friend class PairwiseV2PreparedRunnerSession;
};

class PreparedRunnerEvidenceSession final {
  public:
    PreparedRunnerEvidenceSession(const PreparedRunnerEvidenceSession&) = delete;
    PreparedRunnerEvidenceSession(PreparedRunnerEvidenceSession&&) noexcept = default;
    auto operator=(const PreparedRunnerEvidenceSession&) -> PreparedRunnerEvidenceSession& = delete;
    auto operator=(PreparedRunnerEvidenceSession&&) -> PreparedRunnerEvidenceSession& = delete;
    [[nodiscard]] auto identity() const noexcept -> const PreparedEvidenceSessionIdentity& {
        return identity_;
    }
    [[nodiscard]] auto resource() const noexcept
        -> const prepared_resources_v1::PreparedResourceValue& {
        return resource_;
    }
    [[nodiscard]] auto writer() noexcept -> prepared_codec_v1::PreparedCanonicalWriter& {
        return writer_;
    }
    [[nodiscard]] auto sender() noexcept -> PreparedFinalizedEvidenceSender& { return sender_; }

  private:
    PreparedRunnerEvidenceSession(PreparedEvidenceSessionIdentity identity,
                                  prepared_resources_v1::PreparedResourceValue resource,
                                  prepared_codec_v1::PreparedCanonicalWriter writer,
                                  PreparedFinalizedEvidenceSender sender) noexcept
        : identity_{std::move(identity)}, resource_{std::move(resource)},
          writer_{std::move(writer)}, sender_{std::move(sender)} {}
    PreparedEvidenceSessionIdentity identity_;
    prepared_resources_v1::PreparedResourceValue resource_;
    prepared_codec_v1::PreparedCanonicalWriter writer_;
    PreparedFinalizedEvidenceSender sender_;
    friend class PreparedEvidenceSessionBinding;
};

class PreparedControllerEvidenceSession final {
  public:
    PreparedControllerEvidenceSession(const PreparedControllerEvidenceSession&) = delete;
    PreparedControllerEvidenceSession(PreparedControllerEvidenceSession&&) noexcept = default;
    auto operator=(const PreparedControllerEvidenceSession&)
        -> PreparedControllerEvidenceSession& = delete;
    auto operator=(PreparedControllerEvidenceSession&&)
        -> PreparedControllerEvidenceSession& = delete;
    [[nodiscard]] auto identity() const noexcept -> const PreparedEvidenceSessionIdentity& {
        return identity_;
    }
    [[nodiscard]] auto resource() const noexcept
        -> const prepared_resources_v1::PreparedResourceValue& {
        return resource_;
    }
    [[nodiscard]] auto projector() noexcept
        -> results::detail::PreparedPairwiseRunResultProjector& {
        return projector_;
    }
    [[nodiscard]] auto reconstruction() noexcept
        -> prepared_codec_v1::PreparedFinalizedEvidenceReconstruction& {
        return reconstruction_;
    }
    [[nodiscard]] auto receiver() noexcept -> PreparedFinalizedEvidenceReceiver& {
        return receiver_;
    }

  private:
    PreparedControllerEvidenceSession(PreparedEvidenceSessionIdentity identity,
                                      prepared_resources_v1::PreparedResourceValue resource,
                                      results::detail::PreparedPairwiseRunResultProjector projector,
                                      prepared_codec_v1::PreparedFinalizedEvidenceReconstruction reconstruction,
                                      PreparedFinalizedEvidenceReceiver receiver) noexcept
        : identity_{std::move(identity)}, resource_{std::move(resource)},
          projector_{std::move(projector)}, reconstruction_{std::move(reconstruction)},
          receiver_{std::move(receiver)} {}
    PreparedEvidenceSessionIdentity identity_;
    prepared_resources_v1::PreparedResourceValue resource_;
    results::detail::PreparedPairwiseRunResultProjector projector_;
    prepared_codec_v1::PreparedFinalizedEvidenceReconstruction reconstruction_;
    PreparedFinalizedEvidenceReceiver receiver_;
    friend class PreparedEvidenceSessionBinding;
};

class PairwiseV2PreparedControllerSession final {
  public:
    PairwiseV2PreparedControllerSession(const PairwiseV2PreparedControllerSession&) = delete;
    PairwiseV2PreparedControllerSession(PairwiseV2PreparedControllerSession&&) noexcept = default;
    auto operator=(const PairwiseV2PreparedControllerSession&)
        -> PairwiseV2PreparedControllerSession& = delete;
    auto operator=(PairwiseV2PreparedControllerSession&&)
        -> PairwiseV2PreparedControllerSession& = delete;

    [[nodiscard]] static auto prepare(const wire_v1::detail::AuthoredCapacityOffer&,
                                      const PairwiseHostCapacityRequestTranscript&,
                                      stage_b_policy::Selection = stage_b_policy::Selection::existing_caller) noexcept
        -> std::expected<PairwiseV2PreparedControllerSession, PairwiseV2PreparedSessionFailure>;
    [[nodiscard]] auto offer_body() const noexcept -> std::string_view;
    [[nodiscard]] auto retained_startup_body_sizes() const noexcept -> std::array<std::uint64_t, 3>;
    [[nodiscard]] auto accept_ack(const wire_v1::detail::AcceptedCapacityAck&,
                                  std::string_view) noexcept
        -> std::expected<PreparedEvidenceSessionBinding, PairwiseV2PreparedSessionFailure>;

  private:
    PairwiseV2PreparedControllerSession(wire_v1::detail::AuthoredCapacityOffer capacity_offer,
                                        prepared_resources_v1::AuthoredPreparedOffer offer,
                                        PreparedEvidenceSessionBinding binding,
                                        FinalizedEvidenceTransportIdentity pair) noexcept
        : capacity_offer_{std::move(capacity_offer)}, offer_{std::move(offer)},
          binding_{std::move(binding)}, pair_{pair} {}

    wire_v1::detail::AuthoredCapacityOffer capacity_offer_;
    prepared_resources_v1::AuthoredPreparedOffer offer_;
    PreparedEvidenceSessionBinding binding_;
    FinalizedEvidenceTransportIdentity pair_;
    bool completed_{};
};

class PairwiseV2PreparedRunnerSession final {
  public:
    PairwiseV2PreparedRunnerSession(const PairwiseV2PreparedRunnerSession&) = delete;
    PairwiseV2PreparedRunnerSession(PairwiseV2PreparedRunnerSession&&) noexcept = default;
    auto operator=(const PairwiseV2PreparedRunnerSession&)
        -> PairwiseV2PreparedRunnerSession& = delete;
    auto operator=(PairwiseV2PreparedRunnerSession&&) -> PairwiseV2PreparedRunnerSession& = delete;

    [[nodiscard]] static auto accept(std::string_view prepared_offer_body,
                                     const wire_v1::detail::StartupCapacityValue& runner_capacity,
                                     const wire_v1::detail::ParsedCapacityOffer& capacity_offer,
                                     const wire_v1::detail::AcceptedCapacityAck& capacity_ack,
                                     const PairwiseHostCapacityRequestTranscript&,
                                     stage_b_policy::Selection = stage_b_policy::Selection::existing_caller) noexcept
        -> std::expected<PairwiseV2PreparedRunnerSession, PairwiseV2PreparedSessionFailure>;
    [[nodiscard]] auto ack_body() const noexcept -> std::string_view;
    [[nodiscard]] auto retained_startup_body_sizes() const noexcept -> std::array<std::uint64_t, 2>;
    [[nodiscard]] auto complete() noexcept
        -> std::expected<PreparedEvidenceSessionBinding, PairwiseV2PreparedSessionFailure>;

  private:
    PairwiseV2PreparedRunnerSession(prepared_resources_v1::ParsedPreparedOffer offer,
                                    prepared_resources_v1::AcceptedPreparedAck ack,
                                    PreparedEvidenceSessionBinding binding,
                                    FinalizedEvidenceTransportIdentity pair) noexcept
        : offer_{std::move(offer)}, ack_{std::move(ack)}, binding_{std::move(binding)},
          pair_{pair} {}

    prepared_resources_v1::ParsedPreparedOffer offer_;
    prepared_resources_v1::AcceptedPreparedAck ack_;
    PreparedEvidenceSessionBinding binding_;
    FinalizedEvidenceTransportIdentity pair_;
    bool completed_{};
};

} // namespace epycbench::runtime::detail
