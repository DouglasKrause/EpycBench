#pragma once

#include "epycbench/core/digest.hpp"
#include "epycbench/runtime/ipc/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::runtime::detail {

struct PairwiseV2PreparedEvidenceSessionAccess;

inline constexpr std::uint64_t finalized_evidence_raw_chunk_bytes = 786'132U;
inline constexpr std::string_view finalized_evidence_profile =
    "epycbench.finalized-pairwise-evidence/v1";

enum class FinalizedEvidenceTransportFailure : unsigned char {
    invalid_capacity,
    arithmetic_overflow,
    invalid_encoding,
    invalid_envelope,
    unsupported_protocol,
    unexpected_message,
    unsupported_profile,
    sequence_mismatch,
    identity_mismatch,
    invalid_payload,
    frame_over_limit,
    stream_incomplete,
    extra_event,
    digest_mismatch,
    allocation,
    contained_exception,
    contained_unknown_failure,
    io,
    timeout,
    cancelled,
    clean_eof,
    truncated_frame,
    invalid_progress,
    one_use
};

enum class PreparedTransportProgress : unsigned char { pending, complete };
enum class PreparedTransportStage : unsigned char { manifest, chunks, completion, eof };
enum class PreparedTransportSignal : unsigned char { none, timeout, cancelled };

// Fixed callback failures for native post-activation I/O. The last two values
// preserve the existing throwing ByteReader/Writer adapter's containment result.
enum class PreparedIoFailure : unsigned char {
    interrupted, would_block, io, contained_exception, contained_unknown_failure
};
struct PreparedByteWriter final {
    void* context{};
    std::expected<std::size_t, PreparedIoFailure> (*write)(
        void*, std::span<const std::byte>) noexcept{};
};
struct PreparedByteReader final {
    void* context{};
    std::expected<std::size_t, PreparedIoFailure> (*read)(
        void*, std::span<std::byte>) noexcept{};
};

struct FinalizedEvidenceTransportPlan final {
    std::uint64_t maximum_canonical_bytes{};
    std::uint64_t maximum_chunk_count{};

    friend auto operator==(const FinalizedEvidenceTransportPlan&,
                           const FinalizedEvidenceTransportPlan&) -> bool = default;
};

struct FinalizedEvidenceTransportIdentity final {
    ipc::RequestId first;
    ipc::RequestId second;

    friend auto operator==(const FinalizedEvidenceTransportIdentity&,
                           const FinalizedEvidenceTransportIdentity&) -> bool = default;
};

struct FinalizedEvidenceManifest final {
    FinalizedEvidenceTransportIdentity identity;
    std::uint64_t canonical_bytes{};
    std::uint64_t chunk_count{};
    core::Sha256Digest sha256;
};

struct FinalizedEvidenceChunk final {
    FinalizedEvidenceTransportIdentity identity;
    std::uint64_t chunk_count{};
    std::uint64_t chunk_index{};
    std::vector<std::byte> bytes;
};

struct FinalizedEvidenceCompletion final {
    FinalizedEvidenceTransportIdentity identity;
    std::uint64_t canonical_bytes{};
    std::uint64_t chunk_count{};
    core::Sha256Digest sha256;
};

class FinalizedEvidenceTransportReader final {
  public:
    FinalizedEvidenceTransportReader(const FinalizedEvidenceTransportReader&) = delete;
    FinalizedEvidenceTransportReader(FinalizedEvidenceTransportReader&&) noexcept = default;
    auto operator=(const FinalizedEvidenceTransportReader&)
        -> FinalizedEvidenceTransportReader& = delete;
    auto operator=(FinalizedEvidenceTransportReader&&) noexcept
        -> FinalizedEvidenceTransportReader& = default;

    [[nodiscard]] auto read(std::string_view canonical_body) noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto finish() noexcept
        -> std::expected<std::span<const std::byte>, FinalizedEvidenceTransportFailure>;

  private:
    enum class Phase : unsigned char { manifest, chunks, completion, complete, invalid };
    FinalizedEvidenceTransportReader(FinalizedEvidenceTransportPlan plan,
                                     FinalizedEvidenceTransportIdentity identity,
                                     std::vector<std::byte> storage) noexcept
        : plan_{plan}, identity_{identity}, storage_{std::move(storage)} {}

    FinalizedEvidenceTransportPlan plan_;
    FinalizedEvidenceTransportIdentity identity_;
    std::vector<std::byte> storage_;
    Phase phase_{Phase::manifest};
    std::uint64_t canonical_bytes_{};
    std::uint64_t chunk_count_{};
    std::uint64_t next_chunk_{};
    core::Sha256Digest expected_sha256_{core::sha256(std::string_view{})};

    friend struct FinalizedEvidenceTransportAccess;
};

struct FinalizedEvidenceTransportAccess final {
    [[nodiscard]] static auto plan(std::uint64_t maximum_canonical_bytes) noexcept
        -> std::expected<FinalizedEvidenceTransportPlan, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] static auto create_reader(FinalizedEvidenceTransportPlan,
                                            FinalizedEvidenceTransportIdentity) noexcept
        -> std::expected<FinalizedEvidenceTransportReader, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] static auto encode_manifest(const FinalizedEvidenceManifest&) noexcept
        -> std::expected<std::string, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] static auto encode_chunk(const FinalizedEvidenceChunk&) noexcept
        -> std::expected<std::string, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] static auto encode_completion(const FinalizedEvidenceCompletion&) noexcept
        -> std::expected<std::string, FinalizedEvidenceTransportFailure>;
};

// Inactive allocation-free framing seam. Preparation owns and touches the
// maximum frame storage; activation and every subsequent I/O step are noexcept
// and never grow an owning container. Lifecycle supplies timeout/cancellation
// observations and remains the sole owner of absolute deadlines and polling.
class PreparedFinalizedEvidenceSender final {
  public:
    PreparedFinalizedEvidenceSender(const PreparedFinalizedEvidenceSender&) = delete;
    PreparedFinalizedEvidenceSender(PreparedFinalizedEvidenceSender&&) noexcept = default;
    auto operator=(const PreparedFinalizedEvidenceSender&)
        -> PreparedFinalizedEvidenceSender& = delete;
    auto operator=(PreparedFinalizedEvidenceSender&&) -> PreparedFinalizedEvidenceSender& = delete;

    [[nodiscard]] static auto prepare(FinalizedEvidenceTransportPlan,
                                      FinalizedEvidenceTransportIdentity) noexcept
        -> std::expected<PreparedFinalizedEvidenceSender, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto activate(std::span<const char> canonical_bytes) noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto write_step(ipc::ByteWriter&, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto write_step(PreparedByteWriter, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto committed() const noexcept -> bool { return committed_; }
    [[nodiscard]] auto next_stage() const noexcept -> PreparedTransportStage {
        return next_frame_ == 0U ? PreparedTransportStage::manifest
            : next_frame_ <= chunk_count_ ? PreparedTransportStage::chunks : PreparedTransportStage::completion;
    }

  private:
    PreparedFinalizedEvidenceSender(FinalizedEvidenceTransportPlan plan,
                                    FinalizedEvidenceTransportIdentity identity,
                                    std::vector<std::byte> frame) noexcept
        : plan_{plan}, identity_{identity}, frame_{std::move(frame)} {}
    [[nodiscard]] auto build_next_frame() noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    FinalizedEvidenceTransportPlan plan_;
    FinalizedEvidenceTransportIdentity identity_;
    std::vector<std::byte> frame_;
    std::span<const char> canonical_;
    core::Sha256Digest digest_{core::sha256(std::string_view{})};
    std::size_t frame_size_{};
    std::size_t frame_progress_{};
    std::uint64_t chunk_count_{};
    std::uint64_t next_frame_{};
    bool active_{};
    bool terminal_{};
    bool committed_{};
    friend struct PairwiseV2PreparedEvidenceSessionAccess;
};

class PreparedFinalizedEvidenceReceiver final {
  public:
    PreparedFinalizedEvidenceReceiver(const PreparedFinalizedEvidenceReceiver&) = delete;
    PreparedFinalizedEvidenceReceiver(PreparedFinalizedEvidenceReceiver&&) noexcept = default;
    auto operator=(const PreparedFinalizedEvidenceReceiver&)
        -> PreparedFinalizedEvidenceReceiver& = delete;
    auto operator=(PreparedFinalizedEvidenceReceiver&&)
        -> PreparedFinalizedEvidenceReceiver& = delete;

    [[nodiscard]] static auto prepare(FinalizedEvidenceTransportPlan,
                                      FinalizedEvidenceTransportIdentity) noexcept
        -> std::expected<PreparedFinalizedEvidenceReceiver, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto activate() noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto read_step(ipc::ByteReader&, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto read_step(PreparedByteReader, PreparedTransportSignal) noexcept
        -> std::expected<PreparedTransportProgress, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto finish() noexcept
        -> std::expected<std::span<const char>, FinalizedEvidenceTransportFailure>;
    [[nodiscard]] auto completion_received() const noexcept -> bool { return completion_; }
    [[nodiscard]] auto next_stage() const noexcept -> PreparedTransportStage {
        return completion_ ? PreparedTransportStage::eof : next_frame_ == 0U ? PreparedTransportStage::manifest
            : next_frame_ <= chunk_count_ ? PreparedTransportStage::chunks : PreparedTransportStage::completion;
    }

  private:
    PreparedFinalizedEvidenceReceiver(FinalizedEvidenceTransportPlan plan,
                                      FinalizedEvidenceTransportIdentity identity,
                                      std::vector<std::byte> frame,
                                      std::vector<char> logical) noexcept
        : plan_{plan}, identity_{identity}, frame_{std::move(frame)}, logical_{std::move(logical)} {
    }
    [[nodiscard]] auto consume_frame() noexcept
        -> std::expected<void, FinalizedEvidenceTransportFailure>;
    FinalizedEvidenceTransportPlan plan_;
    FinalizedEvidenceTransportIdentity identity_;
    std::vector<std::byte> frame_;
    std::vector<char> logical_;
    std::size_t frame_progress_{};
    std::size_t frame_target_{4U};
    std::size_t logical_size_{};
    std::uint64_t canonical_bytes_{};
    std::uint64_t chunk_count_{};
    std::uint64_t next_frame_{};
    std::array<char, 64> expected_digest_hex_{};
    bool active_{};
    bool completion_{};
    bool eof_{};
    bool terminal_{};
    friend struct PairwiseV2PreparedEvidenceSessionAccess;
};

} // namespace epycbench::runtime::detail
