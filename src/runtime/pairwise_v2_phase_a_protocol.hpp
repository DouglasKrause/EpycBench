#pragma once

#include "runtime/stage_b_resource_policy.hpp"

#include "runtime/pairwise_monitoring_host_capacity_startup.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <variant>

namespace epycbench::runtime::detail {

struct PairwiseV2PreparedEvidenceSessionAccess;

inline constexpr std::string_view pairwise_cancel_profile =
    "epycbench.pairwise-cancel/v1";
inline constexpr std::string_view pairwise_accepted_profile =
    "epycbench.pairwise-accepted/v1";
inline constexpr std::string_view pairwise_cancelled_profile =
    "epycbench.pairwise-cancelled/v1";

enum class PairwiseV2PhaseAFailure : unsigned char {
    invalid_encoding,
    invalid_envelope,
    unsupported_protocol,
    unexpected_message,
    unsupported_profile,
    sequence_mismatch,
    identity_mismatch,
    invalid_payload,
    invalid_state,
    frame_zero,
    frame_over_limit,
    truncated_frame,
    io_failure,
    allocation,
    contained_exception,
    contained_unknown_failure
};

enum class PairwiseV2CancellationReason : unsigned char {
    user_interrupt,
    controller_shutdown,
    controller_timeout
};

enum class PairwiseV2CancellationPhase : unsigned char {
    startup,
    admission,
    service,
    execution,
    cleanup
};

struct PairwiseV2PairIdentity final {
    ipc::RequestId first;
    ipc::RequestId second;

    friend auto operator==(const PairwiseV2PairIdentity&,
                           const PairwiseV2PairIdentity&) -> bool = default;
};

struct PairwiseV2GateAInputs final {
    PairwiseHostCapacityRequestTranscript request;
    TransportedLinuxMonitoringCapacityCommitment commitment;
};

class PairwiseV2RequestHandoff final {
  public:
    PairwiseV2RequestHandoff(const PairwiseV2RequestHandoff&) = delete;
    PairwiseV2RequestHandoff(PairwiseV2RequestHandoff&&) noexcept;
    auto operator=(const PairwiseV2RequestHandoff&) -> PairwiseV2RequestHandoff& = delete;
    auto operator=(PairwiseV2RequestHandoff&&) -> PairwiseV2RequestHandoff& = delete;

    [[nodiscard]] auto identity() const noexcept -> const PairwiseV2PairIdentity& {
        return identity_;
    }
    [[nodiscard]] auto request() const noexcept
        -> const PairwiseHostCapacityRequestTranscript& {
        return request_;
    }
    [[nodiscard]] auto consume() && noexcept
        -> std::expected<PairwiseV2GateAInputs, PairwiseV2PhaseAFailure>;

  private:
    explicit PairwiseV2RequestHandoff(
        PairwiseHostCapacityRequestTranscript request,
        TransportedLinuxMonitoringCapacityCommitment commitment) noexcept
        : identity_{request.first_request_id(), request.second_request_id()},
          request_{std::move(request)}, commitment_{std::move(commitment)} {}

    PairwiseV2PairIdentity identity_;
    PairwiseHostCapacityRequestTranscript request_;
    TransportedLinuxMonitoringCapacityCommitment commitment_;
    bool active_{true};

    friend struct PairwiseV2PhaseAProtocolAccess;
};

class PairwiseV2CancellationReceipt final {
  public:
    PairwiseV2CancellationReceipt(const PairwiseV2CancellationReceipt&) = delete;
    PairwiseV2CancellationReceipt(PairwiseV2CancellationReceipt&&) noexcept;
    auto operator=(const PairwiseV2CancellationReceipt&)
        -> PairwiseV2CancellationReceipt& = delete;
    auto operator=(PairwiseV2CancellationReceipt&&)
        -> PairwiseV2CancellationReceipt& = delete;

    [[nodiscard]] auto identity() const noexcept -> const PairwiseV2PairIdentity& {
        return identity_;
    }
    [[nodiscard]] auto reason() const noexcept -> PairwiseV2CancellationReason {
        return reason_;
    }

  private:
    PairwiseV2CancellationReceipt(PairwiseV2PairIdentity identity,
                                  PairwiseV2CancellationReason reason) noexcept
        : identity_{identity}, reason_{reason}, active_{true} {}
    PairwiseV2PairIdentity identity_;
    PairwiseV2CancellationReason reason_{};
    bool active_{};
    friend struct PairwiseV2PhaseAProtocolAccess;
};

struct PairwiseV2Accepted final {
    PairwiseV2PairIdentity identity;
};

struct PairwiseV2Cancelled final {
    PairwiseV2PairIdentity identity;
    PairwiseV2CancellationReason reason{};
    PairwiseV2CancellationPhase phase{};
};

class PairwiseV2ControlWriterState final {
  public:
    PairwiseV2ControlWriterState(const PairwiseV2ControlWriterState&) = delete;
    PairwiseV2ControlWriterState(PairwiseV2ControlWriterState&&) noexcept;
    auto operator=(const PairwiseV2ControlWriterState&)
        -> PairwiseV2ControlWriterState& = delete;
    auto operator=(PairwiseV2ControlWriterState&&)
        -> PairwiseV2ControlWriterState& = delete;

  private:
    enum class Phase : unsigned char {
        start,
        request_write_started,
        requested,
        requested_acknowledged,
        cancel_write_started,
        cancelled,
        cancelled_acknowledged,
        invalid
    };
    PairwiseV2ControlWriterState(
        PairwiseV2PairIdentity identity,
        PairwiseHostCapacityOfferValidationState validation) noexcept
        : identity_{identity}, validation_{std::move(validation)} {}
    Phase phase_{Phase::start};
    PairwiseV2PairIdentity identity_;
    PairwiseHostCapacityOfferValidationState validation_;
    std::optional<PairwiseV2CancellationReason> cancellation_reason_;
    bool acknowledged_{};

    friend struct PairwiseV2PhaseAProtocolAccess;
    friend struct PairwiseV2SyntheticProtocolAccess;
    friend struct PairwiseV2NativeProtocolAccess;
};

class PairwiseV2ControlReaderState final {
  public:
    PairwiseV2ControlReaderState(const PairwiseV2ControlReaderState&) = delete;
    PairwiseV2ControlReaderState(PairwiseV2ControlReaderState&&) noexcept;
    auto operator=(const PairwiseV2ControlReaderState&)
        -> PairwiseV2ControlReaderState& = delete;
    auto operator=(PairwiseV2ControlReaderState&&)
        -> PairwiseV2ControlReaderState& = delete;

  private:
    enum class Phase : unsigned char { start, requested, cancelled, invalid };
    explicit PairwiseV2ControlReaderState(
        TransportedLinuxMonitoringCapacityCommitment commitment) noexcept
        : commitment_{std::move(commitment)} {}
    Phase phase_{Phase::start};
    std::optional<PairwiseV2PairIdentity> identity_;
    std::optional<TransportedLinuxMonitoringCapacityCommitment> commitment_;

    friend struct PairwiseV2PhaseAProtocolAccess;
    friend struct PairwiseV2SyntheticProtocolAccess;
    friend struct PairwiseV2NativeProtocolAccess;
};

class PairwiseV2EventWriterState final {
  public:
    PairwiseV2EventWriterState(const PairwiseV2EventWriterState&) = delete;
    PairwiseV2EventWriterState(PairwiseV2EventWriterState&&) noexcept;
    auto operator=(const PairwiseV2EventWriterState&)
        -> PairwiseV2EventWriterState& = delete;
    auto operator=(PairwiseV2EventWriterState&&)
        -> PairwiseV2EventWriterState& = delete;

  private:
    enum class Phase : unsigned char {
        start,
        accepted_write_started,
        accepted,
        terminal_write_started,
        terminal,
        invalid
    };
    explicit PairwiseV2EventWriterState(PairwiseV2PairIdentity identity) noexcept
        : identity_{identity} {}
    Phase phase_{Phase::start};
    PairwiseV2PairIdentity identity_;

    friend struct PairwiseV2PhaseAProtocolAccess;
    friend struct PairwiseV2SyntheticProtocolAccess;
    friend struct PairwiseV2NativeProtocolAccess;
    friend struct PairwiseV2PreparedEvidenceSessionAccess;
};

class PairwiseV2EventReaderState final {
  public:
    PairwiseV2EventReaderState(const PairwiseV2EventReaderState&) = delete;
    PairwiseV2EventReaderState(PairwiseV2EventReaderState&&) noexcept;
    auto operator=(const PairwiseV2EventReaderState&)
        -> PairwiseV2EventReaderState& = delete;
    auto operator=(PairwiseV2EventReaderState&&)
        -> PairwiseV2EventReaderState& = delete;

  private:
    enum class Phase : unsigned char { start, accepted, terminal, invalid };
    PairwiseV2EventReaderState(
        PairwiseV2PairIdentity identity,
        std::optional<PairwiseV2CancellationReason> expected_cancellation) noexcept
        : identity_{identity}, expected_cancellation_{expected_cancellation} {}
    Phase phase_{Phase::start};
    PairwiseV2PairIdentity identity_;
    std::optional<PairwiseV2CancellationReason> expected_cancellation_;

    friend struct PairwiseV2PhaseAProtocolAccess;
    friend struct PairwiseV2SyntheticProtocolAccess;
    friend struct PairwiseV2NativeProtocolAccess;
    friend struct PairwiseV2PreparedEvidenceSessionAccess;
};

struct PairwiseV2ControllerAcceptedHandoff final {
    PairwiseV2ControlWriterState control;
    PairwiseV2EventReaderState event;
};

struct PairwiseV2ControllerRejected final {
    PairwiseV2PairIdentity identity;
    PairwiseHostCapacityRejectionReason reason{};
};

using PairwiseV2ControllerStartupOutcome =
    std::variant<PairwiseV2ControllerAcceptedHandoff, PairwiseV2ControllerRejected>;

class PairwiseV2ControlWriteCapability final {
  public:
    PairwiseV2ControlWriteCapability(const PairwiseV2ControlWriteCapability&) = delete;
    PairwiseV2ControlWriteCapability(PairwiseV2ControlWriteCapability&& other) noexcept;
    auto operator=(const PairwiseV2ControlWriteCapability&)
        -> PairwiseV2ControlWriteCapability& = delete;
    auto operator=(PairwiseV2ControlWriteCapability&&)
        -> PairwiseV2ControlWriteCapability& = delete;

  private:
    enum class Kind : unsigned char { request, cancel };
    PairwiseV2ControlWriteCapability(std::string body,
                                     PairwiseV2ControlWriterState owner,
                                     Kind kind) noexcept
        : body_{std::move(body)}, state_{std::move(owner)}, kind_{kind}, active_{true} {}
    std::string body_;
    PairwiseV2ControlWriterState state_;
    Kind kind_{};
    bool active_{};
    friend struct PairwiseV2PhaseAProtocolAccess;
};

class PairwiseV2EventWriteCapability final {
  public:
    PairwiseV2EventWriteCapability(const PairwiseV2EventWriteCapability&) = delete;
    PairwiseV2EventWriteCapability(PairwiseV2EventWriteCapability&& other) noexcept;
    auto operator=(const PairwiseV2EventWriteCapability&)
        -> PairwiseV2EventWriteCapability& = delete;
    auto operator=(PairwiseV2EventWriteCapability&&)
        -> PairwiseV2EventWriteCapability& = delete;

  private:
    enum class Kind : unsigned char { accepted, cancelled };
    PairwiseV2EventWriteCapability(std::string body,
                                   PairwiseV2EventWriterState owner,
                                   Kind kind) noexcept
        : body_{std::move(body)}, state_{std::move(owner)}, kind_{kind}, active_{true} {}
    std::string body_;
    PairwiseV2EventWriterState state_;
    Kind kind_{};
    bool active_{};
    friend struct PairwiseV2PhaseAProtocolAccess;
};

struct PairwiseV2PhaseAProtocolAccess final {
    [[nodiscard]] static auto write_startup_offer(
        ipc::ByteWriter&, AuthoredPairwiseHostCapacityOffer) noexcept
        -> std::expected<PairwiseV2ControlWriterState, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_startup_offer(ipc::ByteReader&,
        stage_b_policy::Selection = stage_b_policy::Selection::existing_caller) noexcept
        -> std::expected<PairwiseV2ControlReaderState, PairwiseV2PhaseAFailure>;
    // Provisional transported identity only; does not attest local admission.
    [[nodiscard]] static auto provisional_identity(const PairwiseV2ControlReaderState&) noexcept
        -> std::optional<PairwiseV2PairIdentity>;
    [[nodiscard]] static auto write_startup_accepted_ack(
        ipc::ByteWriter&, HostCapacityAcceptedAckCapability) noexcept
        -> std::expected<PairwiseV2EventWriterState, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_startup_accepted_ack(
        ipc::ByteReader&, PairwiseV2ControlWriterState,
        const PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority&) noexcept
        -> std::expected<PairwiseV2ControllerAcceptedHandoff,
                         PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_startup_outcome(
        ipc::ByteReader&, PairwiseV2ControlWriterState,
        const PairwiseMonitoringHostCapacityStartupAccess::LowerAuthority&) noexcept
        -> std::expected<PairwiseV2ControllerStartupOutcome,
                         PairwiseV2PhaseAFailure>;

    [[nodiscard]] static auto prepare_request_write(
        PairwiseV2ControlWriterState,
        const PairwiseHostCapacityRequestTranscript&) noexcept
        -> std::expected<PairwiseV2ControlWriteCapability, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto commit_control_write(
        ipc::ByteWriter&, PairwiseV2ControlWriteCapability) noexcept
        -> std::expected<PairwiseV2ControlWriterState, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_request(PairwiseV2ControlReaderState&,
                                           std::string_view) noexcept
        -> std::expected<PairwiseV2RequestHandoff, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto validate_request_control_binding(
        const PairwiseV2RequestHandoff&,
        const PairwiseV2ControlReaderState&) noexcept
        -> std::expected<void, PairwiseV2PhaseAFailure>;

    [[nodiscard]] static auto prepare_cancel_write(
        PairwiseV2ControlWriterState, PairwiseV2CancellationReason) noexcept
        -> std::expected<PairwiseV2ControlWriteCapability, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_cancel(PairwiseV2ControlReaderState&,
                                          std::string_view) noexcept
        -> std::expected<PairwiseV2CancellationReceipt, PairwiseV2PhaseAFailure>;

    [[nodiscard]] static auto bind_committed_cancel(
        PairwiseV2EventReaderState&, const PairwiseV2ControlWriterState&) noexcept
        -> std::expected<void, PairwiseV2PhaseAFailure>;

    [[nodiscard]] static auto prepare_accepted_write(PairwiseV2EventWriterState) noexcept
        -> std::expected<PairwiseV2EventWriteCapability, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto commit_event_write(
        ipc::ByteWriter&, PairwiseV2EventWriteCapability) noexcept
        -> std::expected<PairwiseV2EventWriterState, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_accepted(PairwiseV2EventReaderState&,
                                            std::string_view) noexcept
        -> std::expected<PairwiseV2Accepted, PairwiseV2PhaseAFailure>;

    [[nodiscard]] static auto prepare_cancelled_write(
        PairwiseV2EventWriterState, PairwiseV2CancellationReceipt,
        PairwiseV2CancellationPhase) noexcept
        -> std::expected<PairwiseV2EventWriteCapability, PairwiseV2PhaseAFailure>;
    [[nodiscard]] static auto read_cancelled(PairwiseV2EventReaderState&,
                                             std::string_view) noexcept
        -> std::expected<PairwiseV2Cancelled, PairwiseV2PhaseAFailure>;

};

} // namespace epycbench::runtime::detail
