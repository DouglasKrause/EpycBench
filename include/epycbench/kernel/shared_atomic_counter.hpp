#pragma once

#include "epycbench/kernel/task.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace epycbench::kernel {

inline constexpr std::string_view shared_atomic_counter_profile =
    "epycbench.task.shared-atomic-counter-u64/v1";
inline constexpr std::size_t shared_atomic_counter_ranks = 2U;
inline constexpr std::size_t shared_atomic_counter_stride = 64U;
inline constexpr std::size_t shared_atomic_counter_bytes =
    shared_atomic_counter_ranks * shared_atomic_counter_stride;
inline constexpr std::byte shared_atomic_counter_padding{0xa5};

enum class AtomicCounterPattern { shared_slot, distinct_slots };
enum class AtomicCounterError {
    increment_count_overflow,
    rank_unavailable,
    storage_too_small,
    unaligned_storage,
    invalid_trial
};

struct AtomicCounterWork {
    TaskRepetitionCount team_tasks;
    core::WorkCount increments;
};

[[nodiscard]] auto atomic_counter_work(TaskRepetitionCount count)
    -> std::expected<AtomicCounterWork, AtomicCounterError>;

struct AtomicCounterRankProgress {
    std::size_t rank;
    std::uint64_t completed_increments;
};

class AtomicCounterTrial;

// One transferred callback operand. The configured rank owns this object
// exclusively; its destination can be shared with the other rank.
class AtomicCounterRankOperation {
  public:
    AtomicCounterRankOperation(AtomicCounterRankOperation&&) noexcept;
    AtomicCounterRankOperation(const AtomicCounterRankOperation&) = delete;
    auto operator=(const AtomicCounterRankOperation&) -> AtomicCounterRankOperation& = delete;
    auto operator=(AtomicCounterRankOperation&&) -> AtomicCounterRankOperation& = delete;

    // Timed. Performs R relaxed, lock-free increments without retaining an
    // atomic_ref. The loop does not poll for cancellation.
    [[nodiscard]] auto execute() && noexcept -> std::optional<AtomicCounterRankProgress>;

  private:
    friend class AtomicCounterTrial;
    AtomicCounterRankOperation(std::uint64_t& destination, std::size_t rank,
                               TaskRepetitionCount count) noexcept
        : destination_{&destination}, rank_{rank}, count_{count} {}

    std::uint64_t* destination_;
    std::size_t rank_;
    TaskRepetitionCount count_;
};

class AtomicCounterStorage;

class AtomicCounterTrial {
  public:
    AtomicCounterTrial(AtomicCounterTrial&&) noexcept = default;
    AtomicCounterTrial(const AtomicCounterTrial&) = delete;
    auto operator=(const AtomicCounterTrial&) -> AtomicCounterTrial& = delete;
    auto operator=(AtomicCounterTrial&&) -> AtomicCounterTrial& = delete;

    [[nodiscard]] auto pattern() const noexcept -> AtomicCounterPattern { return pattern_; }
    [[nodiscard]] auto work() const noexcept -> AtomicCounterWork { return work_; }
    [[nodiscard]] auto take_rank(std::size_t rank)
        -> std::expected<AtomicCounterRankOperation, AtomicCounterError>;

  private:
    friend class AtomicCounterStorage;
    AtomicCounterTrial(std::uint64_t& first, std::uint64_t& second, AtomicCounterPattern pattern,
                       AtomicCounterWork work, std::uint64_t generation) noexcept
        : first_{&first}, pattern_{pattern}, work_{work}, generation_{generation},
          pending_{AtomicCounterRankOperation{first, 0U, work.team_tasks},
                   AtomicCounterRankOperation{second, 1U, work.team_tasks}} {}

    const std::uint64_t* first_;
    AtomicCounterPattern pattern_;
    AtomicCounterWork work_;
    std::uint64_t generation_;
    std::array<AtomicCounterRankOperation, shared_atomic_counter_ranks> pending_;
};

using AtomicCounterRanks =
    std::array<std::optional<AtomicCounterRankProgress>, shared_atomic_counter_ranks>;

struct AtomicCounterVerification {
    AtomicCounterPattern pattern;
    AtomicCounterWork requested;
    AtomicCounterRanks ranks;
    std::array<std::uint64_t, shared_atomic_counter_ranks> counters;
    bool padding_intact;

    [[nodiscard]] auto matched() const noexcept -> bool;
};

// Owns the language-level lifetimes of two uint64_t objects in caller-owned
// storage. The caller supplies writable storage with no live counter objects
// and keeps it mapped, immobile and exclusively owned until this owner retires.
// Quiesce both workers and retire their executable borrows before verifying,
// preparing another trial or destroying the owner. Rank outcomes supplied to
// verify must belong to the caller's current, quiesced worker generation.
class AtomicCounterStorage {
  public:
    [[nodiscard]] static auto create(std::span<std::byte> bytes)
        -> std::expected<AtomicCounterStorage, AtomicCounterError>;
    ~AtomicCounterStorage() noexcept;
    AtomicCounterStorage(AtomicCounterStorage&&) noexcept;
    AtomicCounterStorage(const AtomicCounterStorage&) = delete;
    auto operator=(const AtomicCounterStorage&) -> AtomicCounterStorage& = delete;
    auto operator=(AtomicCounterStorage&&) -> AtomicCounterStorage& = delete;

    // Untimed and exclusive. Restore both counters and padding.
    [[nodiscard]] auto prepare(AtomicCounterPattern, TaskRepetitionCount)
        -> std::expected<AtomicCounterTrial, AtomicCounterError>;
    [[nodiscard]] auto owns(const AtomicCounterTrial&) const noexcept -> bool;
    // Untimed after both workers quiesce. A partial outcome still retains the
    // factual words and padding, and cannot match the complete requested work.
    [[nodiscard]] auto verify(const AtomicCounterTrial&, AtomicCounterRanks) const noexcept
        -> std::optional<AtomicCounterVerification>;

  private:
    AtomicCounterStorage(std::span<std::byte> bytes, std::uint64_t& first,
                         std::uint64_t& second) noexcept
        : bytes_{bytes}, counters_{&first, &second} {}

    std::span<std::byte> bytes_;
    std::array<std::uint64_t*, shared_atomic_counter_ranks> counters_;
    std::uint64_t generation_{};
};

} // namespace epycbench::kernel
