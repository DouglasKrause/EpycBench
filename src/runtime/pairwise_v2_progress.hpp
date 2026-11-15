#pragma once

#include "epycbench/core/digest.hpp"
#include "runtime/finalized_evidence_prepared_direct_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace epycbench::runtime { struct ServiceThreadReadyEvidence; }
namespace epycbench::runtime::detail {
class PairwiseTypedOwnerTransaction;
class PreparedPairwiseRunnerExecution;
namespace progress {
inline constexpr std::string_view profile = "epycbench.pairwise-progress/v1";
inline constexpr std::size_t chunk_bytes = 256U;
using Digest = std::array<std::byte, 32>;
enum class Kind : std::uint8_t {
    preparation, calibration, warmup, block_opened, observation, failed_observation,
    block_closed, stopping, finalization, terminal, failure, count
};
inline constexpr std::size_t kind_count = static_cast<std::size_t>(Kind::count);
enum class Error : std::uint8_t {
    state, limits, overflow, capacity, allocation, encoding, framing, identity,
    sequence, digest, schema, receipt, cancelled, quiescence
};
enum class Message : std::uint8_t { offer, echo, begin, chunk, end, ack };
enum class Durability : std::uint8_t { process, persistent_block };
enum class Platform : std::uint8_t { injected, native };
enum class Utc : std::uint8_t { supplied, acquired };
enum class Receipt : std::uint8_t { complete_write, synchronized_boundary };
struct Identity {
    std::array<char, 64> session{};
    std::uint64_t first{}, second{};
    Digest offer{};
    auto operator==(const Identity&) const -> bool = default;
};
struct ComponentLimit {
    std::uint64_t count{}, bytes{};
    auto operator==(const ComponentLimit&) const -> bool = default;
};
struct Limits {
    std::array<ComponentLimit, kind_count> components{};
    std::uint64_t transitions{}, arena_bytes{}, wire_bytes{};
    auto operator==(const Limits&) const -> bool = default;
};
struct Offer {
    Identity identity;
    Digest resource{};
    Limits limits;
    Durability durability{Durability::process};
    Platform platform{Platform::injected};
    Utc utc{Utc::supplied};
    auto operator==(const Offer&) const -> bool = default;
};
struct Configuration {
    Limits limits;
    Durability durability{Durability::process};
    Platform platform{Platform::injected};
    Utc utc{Utc::supplied};
    auto operator==(const Configuration&) const -> bool = default;
};
struct Payload {
    Kind kind{Kind::preparation};
    std::uint64_t ordinal{}, operation{}, availability{}, epoch{}, bytes{}, chunks{},
        index{}, first_record{}, last_record{};
    Digest sha256{}, journal_digest{};
    Receipt receipt{Receipt::complete_write};
    std::array<std::byte, chunk_bytes> data{};
    std::size_t data_size{};
};
struct Frame {
    Message message{};
    Offer offer;
    Payload payload;
};
struct EncodedFrame {
    std::array<std::byte, 4100> storage{};
    std::size_t size{};
    [[nodiscard]] auto bytes() const noexcept -> std::span<const std::byte> {
        return {storage.data(), size};
    }
};
[[nodiscard]] auto kind_name(Kind) noexcept -> std::string_view;
[[nodiscard]] auto identity(std::string_view session, std::uint64_t first,
    std::uint64_t second) noexcept -> std::expected<Identity, Error>;
// Checked linear per-kind sums. No allocation, environment acquisition or defaults.
[[nodiscard]] auto limits(std::uint64_t blocks,
    const std::array<std::uint64_t, kind_count>& byte_ceilings) noexcept
    -> std::expected<Limits, Error>;
// Shared finalized-child grammar bounds only. Preparation and compact block
// closure have their own progress grammar; neither is a fabricated root child.
[[nodiscard]] auto validate_component_ceilings(const Limits&,
    const wire_v1::CalculatedEvidenceCapacity&) noexcept -> std::expected<void, Error>;
[[nodiscard]] auto bind_offer(Offer) noexcept -> std::expected<Offer, Error>;
[[nodiscard]] auto encode(const Frame&) noexcept -> std::expected<EncodedFrame, Error>;
[[nodiscard]] auto decode(std::span<const std::byte>) noexcept -> std::expected<Frame, Error>;

// Feed owns at most one frame and never reports a partial body as a message.
class FrameReader final {
  public:
    struct Feed { std::size_t consumed{}; bool ready{}; };
    [[nodiscard]] auto feed(std::span<const std::byte>) noexcept -> std::expected<Feed, Error>;
    [[nodiscard]] auto take() noexcept -> std::expected<Frame, Error>;
    [[nodiscard]] auto finish() const noexcept -> std::expected<void, Error>;
  private:
    EncodedFrame frame_;
    std::size_t target_{4};
    bool failed_{};
};

struct Transition {
    Kind kind{};
    std::uint64_t ordinal{}, operation{}, availability{}, epoch{};
    Digest digest{};
    std::span<const char> canonical;
};
struct Acknowledgement {
    std::uint64_t ordinal{}, first_record{}, last_record{};
    Digest component{}, journal{};
    Receipt receipt{Receipt::complete_write};
};
// Source slice is borrowed from the immutable cumulative arena until retirement.
class Sender final {
  public:
    explicit Sender(Offer) noexcept;
    [[nodiscard]] auto begin(const Transition&) noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto next() noexcept -> std::expected<EncodedFrame, Error>;
    // Native writer calls only after every byte of next() has been transferred.
    [[nodiscard]] auto transferred() noexcept -> std::expected<void, Error>;
    [[nodiscard]] auto acknowledge(const Frame&) noexcept -> std::expected<void, Error>;
    void cancel() noexcept;
    [[nodiscard]] auto waiting() const noexcept -> bool;
    [[nodiscard]] auto idle() const noexcept -> bool;
    [[nodiscard]] auto partial() const noexcept -> bool;
    [[nodiscard]] auto sent_bytes() const noexcept -> std::uint64_t { return wire_; }
  private:
    enum class State : unsigned char { idle, begin, chunks, end, waiting, stopped, failed };
    Offer offer_;
    Transition transition_;
    std::array<std::uint64_t, kind_count> counts_{};
    std::uint64_t ordinal_{1}, index_{}, wire_{}, bytes_{}, last_record_{};
    std::size_t pending_size_{};
    State state_{State::idle};
    bool cancelled_{};
};

class Receiver final {
  public:
    Receiver(Receiver&&) noexcept;
    Receiver(const Receiver&) = delete;
    auto operator=(Receiver&&) -> Receiver& = delete;
    [[nodiscard]] static auto prepare(Offer) noexcept -> std::expected<Receiver, Error>;
    [[nodiscard]] auto accept(const Frame&) noexcept -> std::expected<bool, Error>;
    [[nodiscard]] auto transition() const noexcept -> std::expected<Transition, Error>;
    [[nodiscard]] auto acknowledgement(const Acknowledgement&) noexcept
        -> std::expected<EncodedFrame, Error>;
    [[nodiscard]] auto acknowledged() noexcept -> std::expected<void, Error>;
    void stop() noexcept;
    [[nodiscard]] auto finish() const noexcept -> std::expected<void, Error>;
  private:
    explicit Receiver(Offer, std::vector<char>) noexcept;
    enum class State : unsigned char { idle, chunks, ended, acknowledging, stopped, failed };
    Offer offer_;
    std::vector<char> buffer_;
    std::array<std::uint64_t, kind_count> counts_{};
    Payload pending_;
    std::uint64_t ordinal_{1}, index_{}, bytes_{}, wire_{}, last_record_{};
    std::uint64_t last_availability_{}, last_operation_{}, last_epoch_{};
    std::size_t filled_{};
    State state_{State::idle};
};
} // namespace progress

// Stable prepared backing. Only the selected actual typed owner issues/invalidates
// operation and retirement evidence. No JSON or caller-provided bool can mint it.
class PairwiseProgressState final {
  public:
    struct Receipt {
        const PairwiseProgressState* owner{};
        std::uint64_t epoch{}, operation{}, availability{};
        bool retired{};
      private:
        Receipt(const PairwiseProgressState*, std::uint64_t, std::uint64_t,
                std::uint64_t, bool) noexcept;
        friend class PairwiseProgressState;
    };
    PairwiseProgressState() noexcept = default;
    PairwiseProgressState(const PairwiseProgressState&) = delete;
    auto operator=(const PairwiseProgressState&) -> PairwiseProgressState& = delete;
    [[nodiscard]] auto take() noexcept -> std::expected<Receipt, progress::Error>;
    [[nodiscard]] auto consume(const Receipt&) noexcept -> bool;
    [[nodiscard]] auto valid(const Receipt&) const noexcept -> bool;
    [[nodiscard]] auto retired() const noexcept -> bool { return retired_; }
    [[nodiscard]] auto operation() const noexcept -> std::uint64_t { return operation_; }
  private:
    void close_receipts() noexcept { failed_ = true; quiescent_ = false; leased_ = false; }
    [[nodiscard]] auto bind(const void*) noexcept -> bool;
    [[nodiscard]] auto begin_operation(const void*) noexcept -> bool;
    void invalidate(const void*) noexcept;
    void quiesce(const void*) noexcept;
    void seal(const void*) noexcept;
    const void* issuer_{};
    std::uint64_t epoch_{1}, operation_{}, availability_{};
    bool quiescent_{}, retired_{}, leased_{}, failed_{};
    friend class PairwiseTypedOwnerTransaction;
    friend class PreparedPairwiseRunnerExecution;
};
} // namespace epycbench::runtime::detail

namespace epycbench::runtime::detail {
class PairwisePreparationGraph;
struct PreparationGraphUsage;
struct PreparationGraphLimits;
namespace progress {
// Progress-only grammar bounds from dormant, already reserved graph stocks.
[[nodiscard]] auto preparation_ceiling(const PreparationGraphUsage&, std::size_t workers,
    const PreparationGraphLimits&) noexcept -> std::expected<std::uint64_t, Error>;
[[nodiscard]] auto preparation_ceiling(const PairwisePreparationGraph&,
    const PreparationGraphLimits&) noexcept -> std::expected<std::uint64_t, Error>;
[[nodiscard]] constexpr auto block_closed_ceiling() noexcept -> std::uint64_t {
    // Four ordered ABBA slots, two quoted u64 identities, longest completion token.
    return sizeof("{\"block_id\":,\"completion\":,\"failed_observation\":,\"observations\":[]}") - 1U +
        2U * 22U + sizeof("\"operationally_incomplete\"") - 1U + 4U * 22U + 3U;
}
// Borrowed actual producer facts: the callback finishes before the producer may
// move the selected destination. No erased payload or successful root is minted.
using Fact = std::variant<const PairwisePreparationGraph*, const PairwiseCalibrationEvidence*,
    const PairwiseWarmupEvidence*, const measurement::MeasurementBlockPlan*,
    const FactualObservationEvidence*, const FailedObservationEvidence*,
    const PairwiseBlockAttemptEvidence*, const RunnerStoppingEvaluationEvidence*,
    const PairwiseFinalizationEvidence*, const NormalRunTerminalEvidence*,
    const OperationalRunTerminalEvidence*>;
struct AvailableFact {
    Kind kind;
    Fact fact;
    std::uint64_t availability;
    const PairwiseProgressState::Receipt& receipt;
    std::uint64_t operation{}; // Source operation, distinct from a later retirement receipt.
};
// Borrowed cumulative slice; previously emitted slices remain unchanged.
[[nodiscard]] auto encode_component(prepared_codec_v1::PreparedCanonicalWriter&,
    const AvailableFact&) noexcept -> std::expected<std::span<const char>, Error>;
[[nodiscard]] auto encode_preparation(prepared_codec_v1::PreparedCanonicalWriter&,
    const AvailableFact&, const ServiceThreadReadyEvidence&) noexcept
    -> std::expected<std::span<const char>, Error>;
struct Callback {
    void* context{};
    // true: continuation acknowledged; false: stop requested; error: handoff
    // failure. Neither stop alternative is a fabricated backend operation error.
    std::expected<bool, Error> (*consume)(void*, const AvailableFact&) noexcept{};
};
struct Publication {
    PairwiseProgressState* state{};
    Callback callback;
    std::uint64_t availability{};
    bool interrupted{};
    std::optional<Error> failure;
    struct Deferred { Kind kind{}; Fact fact{}; std::uint64_t availability{}, operation{}; };
    std::array<Deferred,2> pending{};
    std::size_t pending_count{};
    [[nodiscard]] auto emit(Kind, Fact) noexcept -> bool;
    [[nodiscard]] auto drain() noexcept -> bool;
    [[nodiscard]] auto publish(Kind, Fact, std::uint64_t, std::uint64_t) noexcept -> bool;
};
} // namespace progress
} // namespace epycbench::runtime::detail
