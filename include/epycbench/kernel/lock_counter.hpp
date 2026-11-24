#pragma once

#include "epycbench/kernel/shared_atomic_counter.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view lock_counter_task_id = "task.lock.counter.u64/v1";
inline constexpr std::string_view builtin_lock_tas_id = "builtin-u64-lock-tas/v1";
inline constexpr std::string_view builtin_lock_ttas_id = "builtin-u64-lock-ttas/v1";
inline constexpr std::size_t lock_counter_alignment = 64U;
inline constexpr std::size_t lock_counter_bytes = 128U;
inline constexpr std::array<std::size_t, 4> lock_counter_offsets{0U, 64U, 72U, 80U};
inline constexpr std::byte lock_counter_padding{0xa5};

enum class LockCounterPattern { test_and_set, test_test_and_set };

class LockCounterTrial;

class LockCounterRankOperation {
  public:
    LockCounterRankOperation(LockCounterRankOperation&&) noexcept;
    LockCounterRankOperation(const LockCounterRankOperation&) = delete;
    auto operator=(const LockCounterRankOperation&) -> LockCounterRankOperation& = delete;
    auto operator=(LockCounterRankOperation&&) -> LockCounterRankOperation& = delete;

    [[nodiscard]] auto execute() && noexcept -> std::optional<AtomicCounterRankProgress>;

  private:
    friend class LockCounterTrial;
    LockCounterRankOperation(std::uint64_t& lock, std::uint64_t& total, std::uint64_t& tally,
                             std::size_t rank, LockCounterPattern pattern, TaskRepetitionCount repetitions) noexcept
        : lock_{&lock}, total_{&total}, tally_{&tally}, rank_{rank}, pattern_{pattern}, repetitions_{repetitions} {}

    std::uint64_t* lock_;
    std::uint64_t* total_;
    std::uint64_t* tally_;
    std::size_t rank_;
    LockCounterPattern pattern_;
    TaskRepetitionCount repetitions_;
};

class LockCounterStorage;

class LockCounterTrial {
  public:
    LockCounterTrial(LockCounterTrial&&) noexcept = default;
    LockCounterTrial(const LockCounterTrial&) = delete;
    auto operator=(const LockCounterTrial&) -> LockCounterTrial& = delete;
    auto operator=(LockCounterTrial&&) -> LockCounterTrial& = delete;

    [[nodiscard]] auto pattern() const noexcept -> LockCounterPattern { return pattern_; }
    [[nodiscard]] auto work() const noexcept -> AtomicCounterWork { return work_; }
    [[nodiscard]] auto take_rank(std::size_t rank) -> std::expected<LockCounterRankOperation, AtomicCounterError>;

  private:
    friend class LockCounterStorage;
    LockCounterTrial(const std::array<std::uint64_t*, 4>& words, LockCounterPattern pattern,
                     AtomicCounterWork work, std::uint64_t generation) noexcept
        : lock_{words[0]}, pattern_{pattern}, work_{work}, generation_{generation},
          pending_{LockCounterRankOperation{*words[0], *words[1], *words[2], 0U, pattern, work.team_tasks},
                   LockCounterRankOperation{*words[0], *words[1], *words[3], 1U, pattern, work.team_tasks}} {}

    const std::uint64_t* lock_;
    LockCounterPattern pattern_;
    AtomicCounterWork work_;
    std::uint64_t generation_;
    std::array<LockCounterRankOperation, shared_atomic_counter_ranks> pending_;
};

struct LockCounterVerification {
    LockCounterPattern pattern;
    AtomicCounterWork requested;
    AtomicCounterRanks ranks;
    std::uint64_t lock;
    std::uint64_t protected_total;
    std::array<std::uint64_t, shared_atomic_counter_ranks> protected_rank_counts;
    bool padding_intact;

    [[nodiscard]] auto matched() const noexcept -> bool;
};

// The owner keeps the writable storage mapped and exclusive during
// construction and reset. Join both workers and retire their execute borrows
// before inspecting results, resetting the words or releasing this owner.
class LockCounterStorage {
  public:
    [[nodiscard]] static auto create(std::span<std::byte>) -> std::expected<LockCounterStorage, AtomicCounterError>;
    ~LockCounterStorage() noexcept;
    LockCounterStorage(LockCounterStorage&&) noexcept;
    LockCounterStorage(const LockCounterStorage&) = delete;
    auto operator=(LockCounterStorage&&) -> LockCounterStorage& = delete;
    auto operator=(const LockCounterStorage&) -> LockCounterStorage& = delete;

    [[nodiscard]] auto prepare(LockCounterPattern, TaskRepetitionCount)
        -> std::expected<LockCounterTrial, AtomicCounterError>;
    [[nodiscard]] auto owns(const LockCounterTrial&) const noexcept -> bool;
    [[nodiscard]] auto verify(const LockCounterTrial&, AtomicCounterRanks) const noexcept
        -> std::optional<LockCounterVerification>;

  private:
    LockCounterStorage(std::span<std::byte> bytes, std::array<std::uint64_t*, 4> words) noexcept
        : bytes_{bytes}, words_{words} {}
    std::span<std::byte> bytes_;
    std::array<std::uint64_t*, 4> words_;
    std::uint64_t generation_{};
};
} // namespace epycbench::kernel
