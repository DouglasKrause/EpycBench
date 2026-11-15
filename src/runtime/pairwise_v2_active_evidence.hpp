#pragma once

#include "runtime/pairwise_v2_composed_startup.hpp"
#include "runtime/pairwise_v2_prepared_evidence_session.hpp"

namespace epycbench::runtime::detail {

class PreparedPairwiseRunnerExecution;

enum class PairwiseV2ActiveEvidenceStage : unsigned char {
    ownership, backend, canonical_encoding, transport, reconstruction, results
};

struct PairwiseV2ActiveEvidenceFailure final {
    PairwiseV2ActiveEvidenceStage stage{};
    std::optional<FinalizedEvidenceTransportFailure> transport;
    std::optional<prepared_codec_v1::ActiveCodecFailure> codec;
};

enum class PairwiseV2FakeBackendFailure : unsigned char { failed };
struct PairwiseV2FakeEvidenceBackend final {
    void* context{};
    // The fake produces by moving its preallocated typed ledger. No selected
    // backend, OS operation, clock, or publication capability is provided.
    std::expected<FinalizedPairwiseRunEvidence, PairwiseV2FakeBackendFailure>
        (*produce)(void*, const PairwiseV2AdmittedStartup&){};
};

class PairwiseV2ActiveEvidenceRunner final {
  public:
    PairwiseV2ActiveEvidenceRunner(const PairwiseV2ActiveEvidenceRunner&) = delete;
    PairwiseV2ActiveEvidenceRunner(PairwiseV2ActiveEvidenceRunner&&) noexcept;
    auto operator=(const PairwiseV2ActiveEvidenceRunner&) -> PairwiseV2ActiveEvidenceRunner& = delete;
    auto operator=(PairwiseV2ActiveEvidenceRunner&&) -> PairwiseV2ActiveEvidenceRunner& = delete;
    [[nodiscard]] static auto bind(PairwiseV2RunnerResources) noexcept
        -> std::expected<PairwiseV2ActiveEvidenceRunner, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto next_stage() const noexcept -> PreparedTransportStage { return stream_.next_stage(); }
    [[nodiscard]] auto start(PairwiseV2FakeEvidenceBackend) noexcept
        -> std::expected<void, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto produce(PairwiseV2FakeEvidenceBackend) noexcept
        -> std::expected<void, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto produce(PreparedPairwiseRunnerExecution&) noexcept
        -> std::expected<void, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto retained_production_evidence() const noexcept
        -> const FinalizedPairwiseRunEvidence* { return retain_production_ && pending_ ? &*pending_ : nullptr; }
    [[nodiscard]] auto encode() noexcept
        -> std::expected<void, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto admitted_for_retired_capture() const noexcept
        -> const PairwiseV2AdmittedStartup& { return resources_.admitted(); }
    [[nodiscard]] auto write_step(ipc::ByteWriter&, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto write_step(PreparedByteWriter, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, PairwiseV2ActiveEvidenceFailure>;

  private:
    PairwiseV2ActiveEvidenceRunner(PairwiseV2RunnerResources,
                                   PairwiseV2PreparedEvidenceRunner) noexcept;
    PairwiseV2RunnerResources resources_;
    PairwiseV2PreparedEvidenceRunner stream_;
    std::optional<FinalizedPairwiseRunEvidence> pending_;
    bool retain_production_{};
    enum class Phase : unsigned char { prepared, produced, active, terminal } phase_{};
};

class PairwiseV2ActiveEvidenceController final {
  public:
    PairwiseV2ActiveEvidenceController(const PairwiseV2ActiveEvidenceController&) = delete;
    PairwiseV2ActiveEvidenceController(PairwiseV2ActiveEvidenceController&&) noexcept;
    auto operator=(const PairwiseV2ActiveEvidenceController&)
        -> PairwiseV2ActiveEvidenceController& = delete;
    auto operator=(PairwiseV2ActiveEvidenceController&&)
        -> PairwiseV2ActiveEvidenceController& = delete;
    [[nodiscard]] static auto bind(PairwiseV2ControllerResources) noexcept
        -> std::expected<PairwiseV2ActiveEvidenceController, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto next_stage() const noexcept -> PreparedTransportStage { return stream_.next_stage(); }
    [[nodiscard]] auto start() noexcept -> std::expected<void, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto read_step(ipc::ByteReader&, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto read_step(PreparedByteReader, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, PairwiseV2ActiveEvidenceFailure>;
    // Observation only: completion does not release bytes or permit Results.
    // The later lifecycle uses this boundary to start its completion deadline.
    [[nodiscard]] auto completion_received() const noexcept -> bool;
    // Borrowed from the receiver only after its actual successful finish.
    // This survives reconstruction failure and never retries consumption.
    [[nodiscard]] auto validated_bytes() const noexcept -> std::span<const std::byte> {
        return validated_bytes_;
    }
    [[nodiscard]] auto reconstruct() noexcept
        -> std::expected<void, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto project() noexcept
        -> std::expected<results::PairwiseRunResult, PairwiseV2ActiveEvidenceFailure>;
    [[nodiscard]] auto finish() noexcept
        -> std::expected<results::PairwiseRunResult, PairwiseV2ActiveEvidenceFailure>;

  private:
    PairwiseV2ActiveEvidenceController(PairwiseV2ControllerResources,
                                       PairwiseV2PreparedEvidenceController) noexcept;
    PairwiseV2ControllerResources resources_;
    PairwiseV2PreparedEvidenceController stream_;
    std::span<const std::byte> validated_bytes_;
    std::optional<FinalizedPairwiseRunEvidence> pending_;
    enum class Phase : unsigned char { prepared, active, eof, reconstructed, terminal } phase_{};
};

} // namespace epycbench::runtime::detail
