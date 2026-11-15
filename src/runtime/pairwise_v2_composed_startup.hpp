#pragma once

#include "runtime/pairwise_v2_contextual_admission.hpp"
#include "runtime/pairwise_v2_phase_a_protocol.hpp"
#include "runtime/pairwise_v2_progress.hpp"
#include "runtime/pairwise_v2_prepared_resource_session.hpp"

#include <expected>
#include <memory>

namespace epycbench::runtime::detail {

enum class PairwiseV2AuthoritySource : unsigned char { supplied_operations, native };
[[nodiscard]] auto stage_b_progress_configuration(progress::Limits,
    PairwiseV2AuthoritySource) noexcept -> progress::Configuration;

enum class PairwiseV2ComposedStartupFailure : unsigned char {
    invalid_state,
    preparation,
    host_offer,
    resource_offers,
    request,
    authority,
    contextual_admission,
    capacity_offer,
    prepared_offer,
    host_ack,
    resource_acks,
    capacity_ack,
    prepared_ack,
    pairwise_accepted,
    channel_write,
    identity,
};

// Fixed-size commitment to every accepted session/request/resource identity.
// Used only as an ownership witness; it is never derived from evidence bytes.
[[nodiscard]] auto pairwise_v2_resource_identity_commitment(
    const PreparedEvidenceSessionIdentity&) noexcept -> core::Sha256Digest;

// Commits offered bytes only; it does not mint an accepted resource identity.
[[nodiscard]] auto pairwise_v2_offered_resource_commitment(
    std::uint64_t first, std::uint64_t second,
    const std::array<core::Sha256Digest, 4>&) noexcept -> core::Sha256Digest;

namespace test { struct PairwiseV2ResourceTestAccess; }

// Only successful composed startup can create these coherent owning bundles.
// Callers may move the whole bundle, but cannot reassemble its capabilities.
class PairwiseV2ControllerResources final {
  public:
    PairwiseV2ControllerResources(PairwiseV2ControllerResources&&) noexcept = default;
    PairwiseV2ControllerResources(const PairwiseV2ControllerResources&) = delete;
    [[nodiscard]] auto identity() const noexcept -> const PreparedEvidenceSessionIdentity& {
        return evidence.identity();
    }

    [[nodiscard]] auto progress_offer() const noexcept -> const std::optional<progress::Offer>& {
        return progress_;
    }

  private:
    PairwiseV2ControllerResources(PairwiseV2ControllerAcceptedHandoff phase,
                                  PreparedControllerEvidenceSession prepared,
                                  std::vector<core::LinuxCpuId> cpus,
                                  std::optional<progress::Offer> progress_offer = std::nullopt) noexcept
        : phase_a{std::move(phase)}, evidence{std::move(prepared)},
          authenticated_cpus{std::move(cpus)}, authenticated_identity_{pairwise_v2_resource_identity_commitment(evidence.identity())},
          progress_{std::move(progress_offer)} {}
    PairwiseV2ControllerAcceptedHandoff phase_a;
    PreparedControllerEvidenceSession evidence;
    std::vector<core::LinuxCpuId> authenticated_cpus;
    core::Sha256Digest authenticated_identity_;
    std::optional<progress::Offer> progress_;
    friend class PairwiseV2ComposedStartupController;
    friend class PairwiseV2ActiveEvidenceController;
    friend struct test::PairwiseV2ResourceTestAccess;
    friend struct PairwiseV2SyntheticProtocolAccess;
    friend struct PairwiseV2NativeProtocolAccess;
};

class PairwiseV2RunnerResources final {
  public:
    PairwiseV2RunnerResources(PairwiseV2RunnerResources&&) noexcept = default;
    PairwiseV2RunnerResources(const PairwiseV2RunnerResources&) = delete;
    [[nodiscard]] auto identity() const noexcept -> const PreparedEvidenceSessionIdentity& {
        return evidence.identity();
    }
    [[nodiscard]] auto admitted() const noexcept -> const PairwiseV2AdmittedStartup& {
        return admission;
    }

    [[nodiscard]] auto progress_offer() const noexcept -> const std::optional<progress::Offer>& {
        return progress_;
    }

  private:
    PairwiseV2RunnerResources(PairwiseV2ControlReaderState reader,
                              PairwiseV2EventWriterState writer,
                              PairwiseV2AdmittedStartup admitted,
                              PreparedRunnerEvidenceSession prepared,
                                  std::optional<progress::Offer> progress_offer = std::nullopt) noexcept
        : control{std::move(reader)}, events{std::move(writer)},
          admission{std::move(admitted)}, evidence{std::move(prepared)},
          authenticated_identity_{pairwise_v2_resource_identity_commitment(evidence.identity())},
          progress_{std::move(progress_offer)} {}
    PairwiseV2ControlReaderState control;
    PairwiseV2EventWriterState events;
    PairwiseV2AdmittedStartup admission;
    PreparedRunnerEvidenceSession evidence;
    core::Sha256Digest authenticated_identity_;
    std::optional<progress::Offer> progress_;
    friend class PairwiseV2ComposedStartupRunner;
    friend class PairwiseV2ActiveEvidenceRunner;
    friend struct test::PairwiseV2ResourceTestAccess;
    friend struct PairwiseV2SyntheticProtocolAccess;
    friend struct PairwiseV2NativeProtocolAccess;
};

using PairwiseV2ControllerResourceOutcome =
    std::variant<PairwiseV2ControllerResources, PairwiseV2ControllerRejected>;
using PairwiseV2RunnerResourceOutcome =
    std::variant<PairwiseV2RunnerResources, PairwiseV2ControllerRejected>;

// Inactive startup composition for the fake lifecycle. Preparation buffers and
// validates all four control frames and owns all controller resources before
// spawn. No protocol state or role resource escapes until the actual writes
// and corresponding acknowledgement validation have completed.
class PairwiseV2ComposedStartupController final {
  public:
    PairwiseV2ComposedStartupController(const PairwiseV2ComposedStartupController&) = delete;
    PairwiseV2ComposedStartupController(PairwiseV2ComposedStartupController&&) noexcept;
    auto operator=(const PairwiseV2ComposedStartupController&)
        -> PairwiseV2ComposedStartupController& = delete;
    auto operator=(PairwiseV2ComposedStartupController&&)
        -> PairwiseV2ComposedStartupController& = delete;
    ~PairwiseV2ComposedStartupController() noexcept;

    [[nodiscard]] static auto prepare(
        wire_v1::detail::StartupSessionId,
        const PairwiseHostCapacityRequestTranscript&, const RunnerContext&,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        std::optional<core::Sha256Digest>* captured_host_offer = nullptr,
        std::optional<progress::Configuration> progress_profile = std::nullopt,
        stage_b_policy::Selection policy = stage_b_policy::Selection::existing_caller,
        PairwiseV2AuthoritySource = PairwiseV2AuthoritySource::supplied_operations) noexcept
        -> std::expected<PairwiseV2ComposedStartupController, PairwiseV2ComposedStartupFailure>;
    // Preparation-only borrow. read_acknowledgements destroys this owner; the
    // lifecycle capture must complete its bounded owning copy before start.
    [[nodiscard]] auto prepared_capacity_offer() const noexcept
        -> const wire_v1::detail::AuthoredCapacityOffer*;
    // Offered commitment only; this does not attest acknowledgement acceptance.
    [[nodiscard]] auto progress_before_ack() const noexcept -> const progress::Offer*;
    [[nodiscard]] auto write_control(ipc::ByteWriter&) noexcept
        -> std::expected<void, PairwiseV2ComposedStartupFailure>;
    [[nodiscard]] auto read_acknowledgements(ipc::ByteReader&) noexcept
        -> std::expected<PairwiseV2ControllerResourceOutcome, PairwiseV2ComposedStartupFailure>;

  private:
    struct Storage;
    explicit PairwiseV2ComposedStartupController(std::unique_ptr<Storage>) noexcept;
    std::unique_ptr<Storage> storage_;
};

// The caller supplies only fake channel/context/authority adapters. Admission
// reads host offer, resource offers, and request; Gate A and ordered contextual
// admission/Gate B run before either resource body is acknowledged. The exact
// admitted inputs are retained for the later fake backend handoff.
class PairwiseV2ComposedStartupRunner final {
  public:
    PairwiseV2ComposedStartupRunner(const PairwiseV2ComposedStartupRunner&) = delete;
    PairwiseV2ComposedStartupRunner(PairwiseV2ComposedStartupRunner&&) noexcept;
    auto operator=(const PairwiseV2ComposedStartupRunner&)
        -> PairwiseV2ComposedStartupRunner& = delete;
    auto operator=(PairwiseV2ComposedStartupRunner&&)
        -> PairwiseV2ComposedStartupRunner& = delete;
    ~PairwiseV2ComposedStartupRunner() noexcept;

    [[nodiscard]] static auto admit(
        ipc::ByteReader&, wire_v1::detail::StartupSessionId, ipc::RequestId first_request,
        RunnerContext,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        std::optional<progress::Configuration> progress_profile = std::nullopt,
        stage_b_policy::Selection policy = stage_b_policy::Selection::existing_caller,
        PairwiseV2AuthoritySource = PairwiseV2AuthoritySource::supplied_operations) noexcept
        -> std::expected<PairwiseV2ComposedStartupRunner, PairwiseV2ComposedStartupFailure>;
    // Identities extracted from these existing frames remain provisional until
    // the same local admission and transcript checks used by admit complete.
    [[nodiscard]] static auto admit_from_channel(ipc::ByteReader&, RunnerContext,
        PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        std::optional<progress::Configuration>,
        PairwiseV2AuthoritySource = PairwiseV2AuthoritySource::supplied_operations) noexcept
        -> std::expected<PairwiseV2ComposedStartupRunner, PairwiseV2ComposedStartupFailure>;
    [[nodiscard]] auto production_graph_limits() const noexcept -> const PreparationGraphLimits*;
    [[nodiscard]] auto admitted_before_ack() const noexcept -> const PairwiseV2AdmittedStartup*;
    [[nodiscard]] auto progress_before_ack() const noexcept -> const progress::Offer*;
    [[nodiscard]] auto write_acknowledgements(ipc::ByteWriter&) noexcept
        -> std::expected<PairwiseV2RunnerResourceOutcome, PairwiseV2ComposedStartupFailure>;

  private:
    struct Storage;
    explicit PairwiseV2ComposedStartupRunner(std::unique_ptr<Storage>) noexcept;
    [[nodiscard]] static auto admit_impl(ipc::ByteReader&,
        std::optional<wire_v1::detail::StartupSessionId>, std::optional<ipc::RequestId>,
        RunnerContext, PairwiseMonitoringHostCapacityStartupAccess::LowerOperations,
        std::optional<progress::Configuration>, stage_b_policy::Selection, PairwiseV2AuthoritySource) noexcept
        -> std::expected<PairwiseV2ComposedStartupRunner, PairwiseV2ComposedStartupFailure>;
    [[nodiscard]] static auto reject(HostCapacityRejectedAckCapability) noexcept
        -> std::expected<PairwiseV2ComposedStartupRunner, PairwiseV2ComposedStartupFailure>;
    std::unique_ptr<Storage> storage_;
};

} // namespace epycbench::runtime::detail
