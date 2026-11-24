#include "epycbench/kernel/false_sharing_counter.hpp"

#include "kernel/atomic_counter_operation.hpp"
#include "kernel/counter_trial_generation.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <utility>

namespace epycbench::kernel {
namespace {
using CounterAccess = std::atomic_ref<std::uint64_t>;
static_assert(CounterAccess::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(sizeof(std::uint64_t) == 8U);
static_assert(false_sharing_counter_line_bytes % CounterAccess::required_alignment == 0U);
static_assert(false_sharing_counter_offsets[1] % CounterAccess::required_alignment == 0U);
static_assert(false_sharing_counter_offsets[2] % CounterAccess::required_alignment == 0U);

auto valid(FalseSharingCounterPattern pattern) noexcept -> bool {
    return pattern == FalseSharingCounterPattern::adjacent_words ||
           pattern == FalseSharingCounterPattern::separate_lines;
}

template <class Byte> auto first_padding(std::span<Byte> bytes) noexcept -> std::span<Byte> {
    return bytes.subspan(2U * sizeof(std::uint64_t),
                         false_sharing_counter_line_bytes - 2U * sizeof(std::uint64_t));
}
template <class Byte> auto remaining_padding(std::span<Byte> bytes) noexcept -> std::span<Byte> {
    return bytes.subspan(false_sharing_counter_line_bytes + sizeof(std::uint64_t));
}
void reset_padding(std::span<std::byte> bytes) noexcept {
    std::ranges::fill(first_padding(bytes), false_sharing_counter_padding);
    std::ranges::fill(remaining_padding(bytes), false_sharing_counter_padding);
}
} // namespace

FalseSharingCounterRankOperation::FalseSharingCounterRankOperation(
    FalseSharingCounterRankOperation&& other) noexcept
    : destination_{std::exchange(other.destination_, nullptr)}, rank_{other.rank_},
      count_{other.count_} {}

auto FalseSharingCounterRankOperation::execute() && noexcept
    -> std::optional<AtomicCounterRankProgress> {
    auto* destination = std::exchange(destination_, nullptr);
    if (!destination)
        return std::nullopt;
    detail::increment_atomic_counter(*destination, count_.value());
    return AtomicCounterRankProgress{rank_, count_.value()};
}

auto FalseSharingCounterTrial::take_rank(std::size_t rank)
    -> std::expected<FalseSharingCounterRankOperation, FalseSharingCounterError> {
    if (rank >= pending_.size() || !pending_[rank].destination_)
        return std::unexpected{FalseSharingCounterError::rank_unavailable};
    return std::move(pending_[rank]);
}

auto FalseSharingCounterVerification::matched() const noexcept -> bool {
    const auto count = requested.team_tasks.value();
    if (!padding_intact || !valid(pattern) ||
        count > std::numeric_limits<std::uint64_t>::max() / shared_atomic_counter_ranks ||
        requested.increments.value() != count * shared_atomic_counter_ranks)
        return false;
    for (std::size_t rank{}; rank < ranks.size(); ++rank)
        if (!ranks[rank] || ranks[rank]->rank != rank || ranks[rank]->completed_increments != count)
            return false;
    const auto expected = pattern == FalseSharingCounterPattern::adjacent_words
                              ? std::array{count, count, std::uint64_t{}}
                              : std::array{count, std::uint64_t{}, count};
    return counters == expected;
}

auto FalseSharingCounterStorage::create(std::span<std::byte> bytes)
    -> std::expected<FalseSharingCounterStorage, FalseSharingCounterError> {
    if (bytes.size() < false_sharing_counter_bytes)
        return std::unexpected{FalseSharingCounterError::storage_too_small};
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % false_sharing_counter_line_bytes != 0U)
        return std::unexpected{FalseSharingCounterError::unaligned_storage};
    reset_padding(bytes);
    std::array<std::uint64_t*, 3> counters{};
    for (std::size_t word{}; word < counters.size(); ++word)
        counters[word] = std::construct_at(
            reinterpret_cast<std::uint64_t*>(bytes.data() + false_sharing_counter_offsets[word]),
            std::uint64_t{});
    return FalseSharingCounterStorage{bytes, counters};
}

FalseSharingCounterStorage::~FalseSharingCounterStorage() noexcept {
    if (!counters_[0])
        return;
    for (std::size_t word = counters_.size(); word > 0U; --word)
        std::destroy_at(counters_[word - 1U]);
}

FalseSharingCounterStorage::FalseSharingCounterStorage(FalseSharingCounterStorage&& other) noexcept
    : bytes_{std::exchange(other.bytes_, {})}, counters_{std::exchange(other.counters_, {})},
      generation_{std::exchange(other.generation_, 0U)} {}

auto FalseSharingCounterStorage::prepare(FalseSharingCounterPattern pattern,
                                         TaskRepetitionCount count)
    -> std::expected<FalseSharingCounterTrial, FalseSharingCounterError> {
    if (!counters_[0] || !valid(pattern))
        return std::unexpected{FalseSharingCounterError::invalid_trial};
    const auto work = atomic_counter_work(count);
    if (!work)
        return std::unexpected{work.error()};
    const auto generation = detail::next_counter_trial_generation();
    if (!generation)
        return std::unexpected{FalseSharingCounterError::invalid_trial};
    reset_padding(bytes_);
    for (auto* counter : counters_)
        CounterAccess{*counter}.store(0U, std::memory_order_relaxed);
    auto& second = *counters_[pattern == FalseSharingCounterPattern::adjacent_words ? 1U : 2U];
    generation_ = *generation;
    return FalseSharingCounterTrial{*counters_[0], second, pattern, *work, generation_};
}

auto FalseSharingCounterStorage::owns(const FalseSharingCounterTrial& trial) const noexcept
    -> bool {
    return counters_[0] && trial.first_ == counters_[0] && trial.generation_ == generation_;
}

auto FalseSharingCounterStorage::verify(const FalseSharingCounterTrial& trial,
                                        AtomicCounterRanks ranks) const noexcept
    -> std::optional<FalseSharingCounterVerification> {
    if (!owns(trial))
        return std::nullopt;
    const auto is_padding = [](std::byte value) { return value == false_sharing_counter_padding; };
    const std::span<const std::byte> bytes{bytes_};
    return FalseSharingCounterVerification{
        trial.pattern(),
        trial.work(),
        std::move(ranks),
        {CounterAccess{*counters_[0]}.load(std::memory_order_relaxed),
         CounterAccess{*counters_[1]}.load(std::memory_order_relaxed),
         CounterAccess{*counters_[2]}.load(std::memory_order_relaxed)},
        std::ranges::all_of(first_padding(bytes), is_padding) &&
            std::ranges::all_of(remaining_padding(bytes), is_padding)};
}

} // namespace epycbench::kernel
