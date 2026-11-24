#include "epycbench/kernel/lock_counter.hpp"
#include "kernel/counter_trial_generation.hpp"
#include "kernel/package_cooperative_leaf.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace epycbench::kernel {
namespace {
using Lock = std::atomic_ref<std::uint64_t>;
static_assert(Lock::is_always_lock_free && lock_counter_alignment % Lock::required_alignment == 0U);
static_assert(sizeof(std::uint64_t) == 8U);

auto valid(LockCounterPattern pattern) noexcept -> bool {
    return pattern == LockCounterPattern::test_and_set || pattern == LockCounterPattern::test_test_and_set;
}

template <class Byte> auto gap(std::span<Byte> storage) noexcept {
    return storage.subspan(sizeof(std::uint64_t), lock_counter_offsets[1] - sizeof(std::uint64_t));
}
template <class Byte> auto tail(std::span<Byte> storage) noexcept {
    return storage.subspan(lock_counter_offsets.back() + sizeof(std::uint64_t));
}

template <LockCounterPattern pattern, bool cancellable = false>
auto update(Lock lock, std::uint64_t& total, std::uint64_t& tally, std::uint64_t requested,
            BarrierStopCheck stop = {}) noexcept -> std::uint64_t {
    std::uint64_t completed{};
    while (completed < requested) {
        do {
            if constexpr (cancellable) {
                if (stop.stopped()) return completed;
            }
            if constexpr (pattern == LockCounterPattern::test_test_and_set) {
                while (lock.load(std::memory_order_relaxed) != 0U) {
                    if constexpr (cancellable) {
                        if (stop.stopped()) return completed;
                    }
                }
            }
        } while (lock.exchange(1U, std::memory_order_acquire) != 0U);
        ++total;
        ++tally;
        lock.store(0U, std::memory_order_release);
        ++completed;
    }
    return completed;
}
} // namespace

auto detail::run_packaged_lock_rank(unsigned char* storage, std::size_t rank, std::uint64_t repetitions,
                                    LockCounterPattern pattern, BarrierStopCheck stop) noexcept -> std::uint64_t {
    auto& lock = *reinterpret_cast<std::uint64_t*>(storage + lock_counter_offsets[0]);
    auto& total = *reinterpret_cast<std::uint64_t*>(storage + lock_counter_offsets[1]);
    auto& tally = *reinterpret_cast<std::uint64_t*>(storage + lock_counter_offsets[rank + 2U]);
    return pattern == LockCounterPattern::test_and_set
        ? update<LockCounterPattern::test_and_set, true>(Lock{lock}, total, tally, repetitions, stop)
        : update<LockCounterPattern::test_test_and_set, true>(Lock{lock}, total, tally, repetitions, stop);
}

LockCounterRankOperation::LockCounterRankOperation(LockCounterRankOperation&& other) noexcept
    : lock_{std::exchange(other.lock_, nullptr)}, total_{other.total_}, tally_{other.tally_},
      rank_{other.rank_}, pattern_{other.pattern_}, repetitions_{other.repetitions_} {}

auto LockCounterRankOperation::execute() && noexcept -> std::optional<AtomicCounterRankProgress> {
    auto* word = std::exchange(lock_, nullptr);
    if (!word) return std::nullopt;
    const auto completed = pattern_ == LockCounterPattern::test_and_set
        ? update<LockCounterPattern::test_and_set>(Lock{*word}, *total_, *tally_, repetitions_.value())
        : update<LockCounterPattern::test_test_and_set>(Lock{*word}, *total_, *tally_, repetitions_.value());
    return AtomicCounterRankProgress{rank_, completed};
}

auto LockCounterTrial::take_rank(std::size_t rank) -> std::expected<LockCounterRankOperation, AtomicCounterError> {
    if (rank >= pending_.size() || !pending_[rank].lock_)
        return std::unexpected{AtomicCounterError::rank_unavailable};
    return std::move(pending_[rank]);
}

auto LockCounterVerification::matched() const noexcept -> bool {
    const auto count = requested.team_tasks.value();
    if (!valid(pattern) || count > UINT64_MAX / shared_atomic_counter_ranks || !padding_intact ||
        lock != 0U || requested.increments.value() != shared_atomic_counter_ranks * count ||
        protected_total != requested.increments.value())
        return false;
    for (std::size_t i{}; i < ranks.size(); ++i) {
        if (protected_rank_counts[i] != count || !ranks[i] || ranks[i]->rank != i ||
            ranks[i]->completed_increments != protected_rank_counts[i])
            return false;
    }
    return true;
}

auto LockCounterStorage::create(std::span<std::byte> bytes) -> std::expected<LockCounterStorage, AtomicCounterError> {
    if (bytes.size() < lock_counter_bytes) return std::unexpected{AtomicCounterError::storage_too_small};
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % lock_counter_alignment != 0U)
        return std::unexpected{AtomicCounterError::unaligned_storage};
    std::ranges::fill(bytes, lock_counter_padding);
    std::array<std::uint64_t*, 4> words{};
    for (std::size_t i{}; i < words.size(); ++i)
        words[i] = std::construct_at(reinterpret_cast<std::uint64_t*>(bytes.data() + lock_counter_offsets[i]), std::uint64_t{});
    return LockCounterStorage{bytes, words};
}

LockCounterStorage::LockCounterStorage(LockCounterStorage&& other) noexcept
    : bytes_{std::exchange(other.bytes_, {})}, words_{std::exchange(other.words_, {})},
      generation_{std::exchange(other.generation_, 0U)} {}

LockCounterStorage::~LockCounterStorage() noexcept {
    if (words_[0])
        for (auto* word : words_) std::destroy_at(word);
}

auto LockCounterStorage::prepare(LockCounterPattern pattern, TaskRepetitionCount repetitions)
    -> std::expected<LockCounterTrial, AtomicCounterError> {
    if (!words_[0] || !valid(pattern)) return std::unexpected{AtomicCounterError::invalid_trial};
    const auto work = atomic_counter_work(repetitions);
    if (!work) return std::unexpected{work.error()};
    const auto generation = detail::next_counter_trial_generation();
    if (!generation) return std::unexpected{AtomicCounterError::invalid_trial};
    for (auto* word : words_) *word = 0U;
    std::ranges::fill(gap(bytes_), lock_counter_padding);
    std::ranges::fill(tail(bytes_), lock_counter_padding);
    generation_ = *generation;
    return LockCounterTrial{words_, pattern, *work, generation_};
}

auto LockCounterStorage::owns(const LockCounterTrial& trial) const noexcept -> bool {
    return words_[0] && words_[0] == trial.lock_ && generation_ == trial.generation_;
}

auto LockCounterStorage::verify(const LockCounterTrial& trial, AtomicCounterRanks ranks) const noexcept
    -> std::optional<LockCounterVerification> {
    if (!owns(trial)) return std::nullopt;
    const auto sentinel = [](std::byte value) { return value == lock_counter_padding; };
    return LockCounterVerification{trial.pattern(), trial.work(), std::move(ranks), *words_[0], *words_[1],
        {*words_[2], *words_[3]}, std::ranges::all_of(gap(bytes_), sentinel) && std::ranges::all_of(tail(bytes_), sentinel)};
}
} // namespace epycbench::kernel
