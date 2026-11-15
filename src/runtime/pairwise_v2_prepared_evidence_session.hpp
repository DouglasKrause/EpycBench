#pragma once

#include "runtime/finalized_evidence_transport.hpp"
#include "runtime/pairwise_v2_phase_a_protocol.hpp"

#include <expected>
#include <span>

namespace epycbench::runtime::detail {

class PreparedRunnerEvidenceSession;
class PreparedControllerEvidenceSession;

// Inactive Phase-A successor bridge. These owners consume the accepted event
// state exactly once and bind the prepared success stream to that same ordered
// pair. They neither select v2 nor invoke a backend or public route.
class PairwiseV2PreparedEvidenceRunner final {
  public:
    PairwiseV2PreparedEvidenceRunner(const PairwiseV2PreparedEvidenceRunner&) = delete;
    PairwiseV2PreparedEvidenceRunner(PairwiseV2PreparedEvidenceRunner&&) noexcept = default;
    auto operator=(const PairwiseV2PreparedEvidenceRunner&)
        -> PairwiseV2PreparedEvidenceRunner& = delete;
    auto operator=(PairwiseV2PreparedEvidenceRunner&&)
        -> PairwiseV2PreparedEvidenceRunner& = delete;

    [[nodiscard]] auto activate(std::span<const char>) noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto write_step(ipc::ByteWriter&, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto write_step(PreparedByteWriter, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto next_stage() const noexcept -> PreparedTransportStage { return sender_.next_stage(); }
    [[nodiscard]] auto committed() const noexcept -> bool { return sender_.committed(); }

  private:
    explicit PairwiseV2PreparedEvidenceRunner(PreparedFinalizedEvidenceSender sender) noexcept
        : sender_{std::move(sender)} {}
    PreparedFinalizedEvidenceSender sender_;
    friend struct PairwiseV2PreparedEvidenceSessionAccess;
};

class PairwiseV2PreparedEvidenceController final {
  public:
    PairwiseV2PreparedEvidenceController(const PairwiseV2PreparedEvidenceController&) = delete;
    PairwiseV2PreparedEvidenceController(PairwiseV2PreparedEvidenceController&&) noexcept = default;
    auto operator=(const PairwiseV2PreparedEvidenceController&)
        -> PairwiseV2PreparedEvidenceController& = delete;
    auto operator=(PairwiseV2PreparedEvidenceController&&)
        -> PairwiseV2PreparedEvidenceController& = delete;

    [[nodiscard]] auto activate() noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto read_step(ipc::ByteReader&, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto read_step(PreparedByteReader, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto finish() noexcept
        -> std::expected<std::span<const char>, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto next_stage() const noexcept -> PreparedTransportStage { return receiver_.next_stage(); }
    [[nodiscard]] auto completion_received() const noexcept -> bool {
        return receiver_.completion_received();
    }

  private:
    explicit PairwiseV2PreparedEvidenceController(
        PreparedFinalizedEvidenceReceiver receiver) noexcept
        : receiver_{std::move(receiver)} {}
    PreparedFinalizedEvidenceReceiver receiver_;
    friend struct PairwiseV2PreparedEvidenceSessionAccess;
};

struct PairwiseV2PreparedEvidenceSessionAccess final {
    // Consume accepted event state and its already prepared role transport.
    // These handoffs never allocate a replacement sender or receiver.
    [[nodiscard]] static auto adopt_runner(PairwiseV2EventWriterState&&,
                                           PreparedRunnerEvidenceSession&) noexcept
        -> std::expected<PairwiseV2PreparedEvidenceRunner, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] static auto adopt_controller(PairwiseV2EventReaderState&&,
                                               PreparedControllerEvidenceSession&) noexcept
        -> std::expected<PairwiseV2PreparedEvidenceController, FinalizedEvidenceTransportFailure>;

    [[nodiscard]] static auto prepare_runner(PairwiseV2EventWriterState&&,
                                             FinalizedEvidenceTransportPlan) noexcept
        -> std::expected<PairwiseV2PreparedEvidenceRunner, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] static auto prepare_controller(PairwiseV2EventReaderState&&,
                                                 FinalizedEvidenceTransportPlan) noexcept
        -> std::expected<PairwiseV2PreparedEvidenceController, FinalizedEvidenceTransportFailure>;
};

} // namespace epycbench::runtime::detail
