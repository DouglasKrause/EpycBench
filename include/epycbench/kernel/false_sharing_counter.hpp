#pragma once

#include "epycbench/kernel/shared_atomic_counter.hpp"

namespace epycbench::kernel {

inline constexpr std::string_view false_sharing_counter_task_id =
    "task.atomic.false-sharing.u64/v1";
inline constexpr std::string_view builtin_atomic_adjacent_implementation_id =
    "builtin-u64-atomic-adjacent/v1";
inline constexpr std::string_view builtin_atomic_separated_implementation_id =
    "builtin-u64-atomic-separated/v1";
inline constexpr std::size_t false_sharing_counter_line_bytes = 64U;
inline constexpr std::size_t false_sharing_counter_bytes = 128U;
inline constexpr std::array<std::size_t, 3> false_sharing_counter_offsets{0U, 8U, 64U};
inline constexpr std::byte false_sharing_counter_padding{0xa5};

enum class FalseSharingCounterPattern { adjacent_words, separate_lines };
using FalseSharingCounterError = AtomicCounterError;

class FalseSharingCounterTrial;

class FalseSharingCounterRankOperation {
  public:
    FalseSharingCounterRankOperation(FalseSharingCounterRankOperation&&) noexcept;
    FalseSharingCounterRankOperation(const FalseSharingCounterRankOperation&) = delete;
    auto operator=(FalseSharingCounterRankOperation&&)
        -> FalseSharingCounterRankOperation& = delete;
    auto operator=(const FalseSharingCounterRankOperation&)
        -> FalseSharingCounterRankOperation& = delete;

    // Timed: R relaxed lock-free increments at this rank's distinct address.
    // No reset, reference scan, allocation or cancellation polling in this loop.
    [[nodiscard]] auto execute() && noexcept -> std::optional<AtomicCounterRankProgress>;

  private:
    friend class FalseSharingCounterTrial;
    FalseSharingCounterRankOperation(std::uint64_t& destination, std::size_t rank,
                                     TaskRepetitionCount count) noexcept
        : destination_{&destination}, rank_{rank}, count_{count} {}
    std::uint64_t* destination_;
    std::size_t rank_;
    TaskRepetitionCount count_;
};

class FalseSharingCounterStorage;

class FalseSharingCounterTrial {
  public:
    FalseSharingCounterTrial(FalseSharingCounterTrial&&) noexcept = default;
    FalseSharingCounterTrial(const FalseSharingCounterTrial&) = delete;
    auto operator=(FalseSharingCounterTrial&&) -> FalseSharingCounterTrial& = delete;
    auto operator=(const FalseSharingCounterTrial&) -> FalseSharingCounterTrial& = delete;

    [[nodiscard]] auto pattern() const noexcept -> FalseSharingCounterPattern { return pattern_; }
    [[nodiscard]] auto work() const noexcept -> AtomicCounterWork { return work_; }
    [[nodiscard]] auto take_rank(std::size_t rank)
        -> std::expected<FalseSharingCounterRankOperation, FalseSharingCounterError>;

  private:
    friend class FalseSharingCounterStorage;
    FalseSharingCounterTrial(std::uint64_t& first, std::uint64_t& second,
                             FalseSharingCounterPattern pattern, AtomicCounterWork work,
                             std::uint64_t generation) noexcept
        : first_{&first}, pattern_{pattern}, work_{work}, generation_{generation},
          pending_{FalseSharingCounterRankOperation{first, 0U, work.team_tasks},
                   FalseSharingCounterRankOperation{second, 1U, work.team_tasks}} {}
    const std::uint64_t* first_;
    FalseSharingCounterPattern pattern_;
    AtomicCounterWork work_;
    std::uint64_t generation_;
    std::array<FalseSharingCounterRankOperation, shared_atomic_counter_ranks> pending_;
};

struct FalseSharingCounterVerification {
    FalseSharingCounterPattern pattern;
    AtomicCounterWork requested;
    AtomicCounterRanks ranks;
    // Values in physical offset order 0, 8, 64, including the unused word.
    std::array<std::uint64_t, 3> counters;
    bool padding_intact;
    [[nodiscard]] auto matched() const noexcept -> bool;
};

// Owns three uint64_t lifetimes in caller-owned writable storage. Both patterns
// use this same fixed layout: rank zero uses offset 0, rank one uses 8 or 64.
// This leaf does not establish CPU affinity, NUMA placement or cache geometry.
// Physical false-sharing characterization requires the owning FP18 contract.
// Quiesce all ranks and retire executable borrows before reset, verify or retire.
class FalseSharingCounterStorage {
  public:
    [[nodiscard]] static auto create(std::span<std::byte>)
        -> std::expected<FalseSharingCounterStorage, FalseSharingCounterError>;
    ~FalseSharingCounterStorage() noexcept;
    FalseSharingCounterStorage(FalseSharingCounterStorage&&) noexcept;
    FalseSharingCounterStorage(const FalseSharingCounterStorage&) = delete;
    auto operator=(FalseSharingCounterStorage&&) -> FalseSharingCounterStorage& = delete;
    auto operator=(const FalseSharingCounterStorage&) -> FalseSharingCounterStorage& = delete;

    // Untimed/exclusive: reset all three words and every padding byte.
    [[nodiscard]] auto prepare(FalseSharingCounterPattern, TaskRepetitionCount)
        -> std::expected<FalseSharingCounterTrial, FalseSharingCounterError>;
    [[nodiscard]] auto owns(const FalseSharingCounterTrial&) const noexcept -> bool;
    // Untimed after quiescence. Rank/count completions carry no generation tag:
    // the caller must bind them to this current collective worker generation.
    // Incomplete outcomes preserve raw facts but fail
    // the complete-work reference. Allocation padding is checked, never work.
    [[nodiscard]] auto verify(const FalseSharingCounterTrial&, AtomicCounterRanks) const noexcept
        -> std::optional<FalseSharingCounterVerification>;

  private:
    FalseSharingCounterStorage(std::span<std::byte> bytes,
                               std::array<std::uint64_t*, 3> counters) noexcept
        : bytes_{bytes}, counters_{counters} {}
    std::span<std::byte> bytes_;
    std::array<std::uint64_t*, 3> counters_;
    std::uint64_t generation_{};
};

} // namespace epycbench::kernel
