#pragma once

#include "runtime/pairwise_v2_phase_a_protocol.hpp"
#include "runtime/pairwise_v2_progress.hpp"
#include "runtime/service_platform_operations.hpp"
#include "epycbench/runtime/production_control_monitor.hpp"
#include "platform_linux/process_ipc_operations.hpp"

#include <array>
#include <memory>
#include <utility>

namespace epycbench::runtime::detail {

struct PairwiseV2ControlFrame final {
    std::array<std::byte, 1024> bytes{};
    std::size_t size{4};
    [[nodiscard]] auto view() const noexcept -> std::span<const std::byte> {
        return {bytes.data(), size};
    }
};
[[nodiscard]] auto pairwise_v2_cancel_frame(PairwiseV2PairIdentity, std::size_t reason,
    std::optional<std::size_t> phase) noexcept -> PairwiseV2ControlFrame;

enum class PairwiseV2ControlFailureKind : unsigned char {
    invalid_state, capacity, preparation, placement, native, protocol
};
struct PairwiseV2ControlFailure final {
    PairwiseV2ControlFailureKind kind{};
    std::optional<platform_linux::detail::FixedProcessFailure> native;
};
struct PairwiseV2ControlSnapshot final {
    std::optional<PairwiseV2ControlFailure> failure;
    std::optional<PairwiseV2CancellationReason> cancellation;
    std::optional<PairwiseV2CancellationPhase> first_observation;
    bool eof{}, local_stop{}, joined{};
    std::optional<progress::Frame> acknowledgement{};
};

// One bounded frame per step. The endpoint remains owned by the native runner.
// Cooperative checkpoints stop after cancellation; the placed reader continues
// to detect duplicate/trailing input until explicit local retirement.
class PairwiseV2FixedControlReader final {
  public:
    explicit PairwiseV2FixedControlReader(PairwiseV2PairIdentity,
        std::optional<progress::Offer> = std::nullopt) noexcept;
    [[nodiscard]] auto arm_ack(std::uint64_t ordinal, const progress::Digest&) noexcept -> bool;
    [[nodiscard]] auto take_ack() noexcept -> std::optional<progress::Frame>;
    [[nodiscard]] auto step(platform_linux::LocalIpcEndpoint&, int wait_ms,
        PairwiseV2CancellationPhase observation,
        const std::atomic<PairwiseV2CancellationPhase>* current_phase = nullptr) noexcept -> PairwiseV2ControlSnapshot;
    [[nodiscard]] auto snapshot() const noexcept -> PairwiseV2ControlSnapshot { return result_; }
  private:
    std::array<PairwiseV2ControlFrame, 3> expected_;
    std::array<std::byte, 1028> input_{};
    std::size_t size_{}, target_{4};
    PairwiseV2ControlSnapshot result_;
    std::optional<progress::Offer> progress_;
    struct ExpectedAck { std::uint64_t ordinal; progress::Digest digest; };
    std::optional<ExpectedAck> expected_ack_;
    bool frame_ack_eligible_{};
};

struct PairwiseV2NativeProtocolAccess;
class PairwiseV2ControlReadPermit final {
  public:
    PairwiseV2ControlReadPermit(const PairwiseV2ControlReadPermit&) = delete;
    PairwiseV2ControlReadPermit(PairwiseV2ControlReadPermit&& other) noexcept
        : pair_{other.pair_}, valid_{std::exchange(other.valid_, false)} {}
  private:
    explicit PairwiseV2ControlReadPermit(PairwiseV2PairIdentity pair) noexcept : pair_{pair} {}
    PairwiseV2PairIdentity pair_;
    bool valid_{true};
    friend struct PairwiseV2NativeProtocolAccess;
    friend class PairwiseV2ProductionControlMonitor;
};

class PairwiseV2ProductionControlMonitor final {
  public:
    PairwiseV2ProductionControlMonitor(PairwiseV2ProductionControlMonitor&&) noexcept;
    PairwiseV2ProductionControlMonitor(const PairwiseV2ProductionControlMonitor&) = delete;
    auto operator=(PairwiseV2ProductionControlMonitor&&) -> PairwiseV2ProductionControlMonitor& = delete;
    ~PairwiseV2ProductionControlMonitor() noexcept;
    [[nodiscard]] static auto auxiliary_bytes() noexcept -> std::size_t;
    [[nodiscard]] static auto prepare(const AdmittedPlacementRequest&, PairwiseV2PairIdentity,
        std::shared_ptr<RunnerCancellation>, ServicePlatformOperations, std::size_t limit,
        std::optional<progress::Offer> = std::nullopt) noexcept
        -> std::expected<PairwiseV2ProductionControlMonitor, PairwiseV2ControlFailure>;
    [[nodiscard]] auto start(platform_linux::LocalIpcEndpoint&, PairwiseV2ControlReadPermit) noexcept
        -> std::expected<void, PairwiseV2ControlFailure>;
    // Borrowed command backing remains immutable/live until result or stop_and_join.
    // Only the placed thread reads control and executes this one nonblocking attempt.
    [[nodiscard]] auto submit_progress_end(platform_linux::LocalIpcEndpoint& events,
        std::span<const std::byte> remaining, std::uint64_t ordinal,
        const progress::Digest&) noexcept -> std::expected<void, PairwiseV2ControlFailure>;
    [[nodiscard]] auto take_progress_end_result() noexcept
        -> std::optional<std::expected<std::size_t, PairwiseV2ControlFailure>>;
    // One locked decision: an observed cancellation/failure suppresses continuation.
    [[nodiscard]] auto take_ack() noexcept -> PairwiseV2ControlSnapshot;
    void phase(PairwiseV2CancellationPhase) noexcept;
    // Bounded producer wait; releases the state lock and never reads control.
    [[nodiscard]] auto wait_progress() noexcept -> PairwiseV2ControlSnapshot;
    [[nodiscard]] auto snapshot() const noexcept -> PairwiseV2ControlSnapshot;
    [[nodiscard]] auto readiness() const noexcept -> const ServiceThreadReadyEvidence*;
    [[nodiscard]] auto assignment() const noexcept -> const ServiceThreadAssignment*;
    [[nodiscard]] auto stop_and_join() noexcept -> PairwiseV2ControlSnapshot;
  private:
    struct Storage;
    explicit PairwiseV2ProductionControlMonitor(std::unique_ptr<Storage>) noexcept;
    std::unique_ptr<Storage> storage_;
};

} // namespace epycbench::runtime::detail
