#pragma once

#include "epycbench/kernel/shared_atomic_counter.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view barrier_task_id = "task.barrier.round.u64/v1";
inline constexpr std::string_view builtin_barrier_central_id = "builtin-u64-barrier-central/v1";
inline constexpr std::string_view builtin_barrier_flags_id = "builtin-u64-barrier-flags/v1";
inline constexpr std::size_t barrier_state_alignment = 64U;
inline constexpr std::size_t barrier_state_bytes = 192U;
inline constexpr std::uint64_t barrier_poll_limit = UINT64_C(1) << 22U;
inline constexpr std::byte barrier_padding{0xa5};

enum class BarrierPattern { central_generation, rank_flags };
enum class BarrierRankStatus { complete, cancelled, peer_poll_limit, witness_mismatch };

struct BarrierRankProgress {
    std::size_t rank;
    std::uint64_t completed_rounds;
    std::uint64_t checked_peer_tokens;
    std::uint64_t peer_token_sum;
    BarrierRankStatus status;
};
using BarrierRanks = std::array<std::optional<BarrierRankProgress>, 2>;
struct BarrierState;

struct BarrierStopCheck {
    const void* context{};
    bool (*requested)(const void*) noexcept{};
    [[nodiscard]] auto stopped() const noexcept -> bool { return requested && requested(context); }
};

class BarrierRankOperation {
  public:
    BarrierRankOperation(BarrierRankOperation&&) noexcept;
    BarrierRankOperation(const BarrierRankOperation&) = delete;
    auto operator=(BarrierRankOperation&&) -> BarrierRankOperation& = delete;
    auto operator=(const BarrierRankOperation&) -> BarrierRankOperation& = delete;
    [[nodiscard]] auto execute(BarrierStopCheck = {}) && noexcept -> std::optional<BarrierRankProgress>;

  private:
    friend class BarrierTrial;
    BarrierRankOperation(BarrierState* state, std::size_t rank, BarrierPattern pattern, TaskRepetitionCount rounds,
                         std::uint64_t owner_generation) noexcept
        : state_{state}, rank_{rank}, pattern_{pattern}, rounds_{rounds}, owner_generation_{owner_generation} {}
    BarrierState* state_;
    std::size_t rank_;
    BarrierPattern pattern_;
    TaskRepetitionCount rounds_;
    std::uint64_t owner_generation_;
};

class BarrierTrial {
  public:
    BarrierTrial(BarrierTrial&&) noexcept;
    BarrierTrial(const BarrierTrial&) = delete;
    auto operator=(BarrierTrial&&) -> BarrierTrial& = delete;
    auto operator=(const BarrierTrial&) -> BarrierTrial& = delete;
    [[nodiscard]] auto take_rank(std::size_t rank) -> std::expected<BarrierRankOperation, AtomicCounterError>;
    [[nodiscard]] auto pattern() const noexcept -> BarrierPattern { return pattern_; }
    [[nodiscard]] auto work() const noexcept -> TaskRepetitionCount { return rounds_; }

  private:
    friend class BarrierStorage;
    BarrierTrial(BarrierState* state, BarrierPattern pattern, TaskRepetitionCount rounds,
                 std::uint64_t generation) noexcept
        : state_{state}, pattern_{pattern}, rounds_{rounds}, generation_{generation},
          pending_{BarrierRankOperation{state, 0U, pattern, rounds, generation},
                   BarrierRankOperation{state, 1U, pattern, rounds, generation}} {}
    BarrierState* state_;
    BarrierPattern pattern_;
    TaskRepetitionCount rounds_;
    std::uint64_t generation_;
    std::array<BarrierRankOperation, 2> pending_;
};

struct BarrierVerification {
    BarrierPattern pattern;
    TaskRepetitionCount rounds;
    BarrierRanks ranks;
    std::uint64_t arrivals, generation, cancelled;
    std::array<std::uint64_t, 2> published_generations;
    std::array<std::array<std::uint64_t, 2>, 2> witnesses;
    bool padding_intact;
    [[nodiscard]] auto matched() const noexcept -> bool;
};

// Launch workers only after prepare. The coordinator may cancel the live generation.
// Join the rank operations before verification, reset, destruction or unmapping.
class BarrierStorage {
  public:
    [[nodiscard]] static auto create(std::span<std::byte>) -> std::expected<BarrierStorage, AtomicCounterError>;
    ~BarrierStorage() noexcept;
    BarrierStorage(BarrierStorage&&) noexcept;
    BarrierStorage(const BarrierStorage&) = delete;
    auto operator=(BarrierStorage&&) -> BarrierStorage& = delete;
    auto operator=(const BarrierStorage&) -> BarrierStorage& = delete;
    [[nodiscard]] auto prepare(BarrierPattern, TaskRepetitionCount) -> std::expected<BarrierTrial, AtomicCounterError>;
    [[nodiscard]] auto owns(const BarrierTrial&) const noexcept -> bool;
    void cancel(const BarrierTrial&) noexcept;
    [[nodiscard]] auto verify(const BarrierTrial&, BarrierRanks) const noexcept -> std::optional<BarrierVerification>;

  private:
    BarrierStorage(std::span<std::byte> bytes, BarrierState* state) noexcept : bytes_{bytes}, state_{state} {}
    std::span<std::byte> bytes_;
    BarrierState* state_;
};
} // namespace epycbench::kernel
