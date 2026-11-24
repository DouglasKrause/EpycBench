#pragma once

#include "epycbench/kernel/shared_atomic_counter.hpp"

#include <stop_token>

namespace epycbench::kernel {

inline constexpr std::string_view queue_task_id = "task.queue.spsc.u64/v1";
inline constexpr std::string_view builtin_queue_direct_id = "builtin-u64-spsc-direct/v1";
inline constexpr std::string_view builtin_queue_cached_id = "builtin-u64-spsc-cached/v1";
inline constexpr std::size_t queue_slots = 8U;
inline constexpr std::size_t queue_state_alignment = 64U;
inline constexpr std::size_t queue_state_bytes = 256U;
inline constexpr std::uint64_t queue_poll_limit = UINT64_C(1) << 22U;
inline constexpr std::byte queue_padding{0xa5};

enum class QueuePattern { direct_cursor, cached_cursor };
enum class QueueRankStatus { complete, cancelled, peer_poll_limit, token_mismatch };

struct QueueRankProgress {
    std::size_t rank;
    std::uint64_t transfers;
    std::uint64_t checked_tokens;
    std::uint64_t token_sum;
    QueueRankStatus status;
};
using QueueRanks = std::array<std::optional<QueueRankProgress>, 2>;
struct QueueState;

struct QueueStopCheck {
    const void* context;
    bool (*requested)(const void*) noexcept;
    [[nodiscard]] auto stopped() const noexcept -> bool { return requested && requested(context); }
};

class QueueRankOperation {
  public:
    QueueRankOperation(QueueRankOperation&&) noexcept;
    QueueRankOperation(const QueueRankOperation&) = delete;
    auto operator=(QueueRankOperation&&) -> QueueRankOperation& = delete;
    auto operator=(const QueueRankOperation&) -> QueueRankOperation& = delete;
    [[nodiscard]] auto execute(std::stop_token = {}) && noexcept -> std::optional<QueueRankProgress>;
    [[nodiscard]] auto execute(QueueStopCheck) && noexcept -> std::optional<QueueRankProgress>;

  private:
    friend class QueueTrial;
    QueueRankOperation(QueueState* state, std::size_t rank, QueuePattern pattern, TaskRepetitionCount items,
                         std::uint64_t owner_generation) noexcept
        : state_{state}, rank_{rank}, pattern_{pattern}, items_{items}, owner_generation_{owner_generation} {}
    QueueState* state_;
    std::size_t rank_;
    QueuePattern pattern_;
    TaskRepetitionCount items_;
    std::uint64_t owner_generation_;
};

class QueueTrial {
  public:
    QueueTrial(QueueTrial&&) noexcept;
    QueueTrial(const QueueTrial&) = delete;
    auto operator=(QueueTrial&&) -> QueueTrial& = delete;
    auto operator=(const QueueTrial&) -> QueueTrial& = delete;
    [[nodiscard]] auto take_rank(std::size_t rank) -> std::expected<QueueRankOperation, AtomicCounterError>;
    [[nodiscard]] auto pattern() const noexcept -> QueuePattern { return pattern_; }
    [[nodiscard]] auto work() const noexcept -> TaskRepetitionCount { return items_; }

  private:
    friend class QueueStorage;
    QueueTrial(QueueState* state, QueuePattern pattern, TaskRepetitionCount items,
                 std::uint64_t generation) noexcept
        : state_{state}, pattern_{pattern}, items_{items}, generation_{generation},
          pending_{QueueRankOperation{state, 0U, pattern, items, generation},
                   QueueRankOperation{state, 1U, pattern, items, generation}} {}
    QueueState* state_;
    QueuePattern pattern_;
    TaskRepetitionCount items_;
    std::uint64_t generation_;
    std::array<QueueRankOperation, 2> pending_;
};

struct QueueVerification {
    QueuePattern pattern;
    TaskRepetitionCount items;
    QueueRanks ranks;
    std::uint64_t published, consumed, cancelled;
    std::array<std::uint64_t, queue_slots> slots;
    bool padding_intact;
    [[nodiscard]] auto matched() const noexcept -> bool;
};

// Launch workers only after prepare. The coordinator may cancel the live generation.
// Join the rank operations before verification, reset, destruction or unmapping.
class QueueStorage {
  public:
    [[nodiscard]] static auto create(std::span<std::byte>) -> std::expected<QueueStorage, AtomicCounterError>;
    ~QueueStorage() noexcept;
    QueueStorage(QueueStorage&&) noexcept;
    QueueStorage(const QueueStorage&) = delete;
    auto operator=(QueueStorage&&) -> QueueStorage& = delete;
    auto operator=(const QueueStorage&) -> QueueStorage& = delete;
    [[nodiscard]] auto prepare(QueuePattern, TaskRepetitionCount) -> std::expected<QueueTrial, AtomicCounterError>;
    [[nodiscard]] auto owns(const QueueTrial&) const noexcept -> bool;
    void cancel(const QueueTrial&) noexcept;
    [[nodiscard]] auto verify(const QueueTrial&, QueueRanks) const noexcept -> std::optional<QueueVerification>;

  private:
    QueueStorage(std::span<std::byte> bytes, QueueState* state) noexcept : bytes_{bytes}, state_{state} {}
    std::span<std::byte> bytes_;
    QueueState* state_;
};
} // namespace epycbench::kernel
