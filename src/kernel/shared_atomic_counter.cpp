#include "epycbench/kernel/shared_atomic_counter.hpp"

#include "kernel/atomic_counter_operation.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <utility>

namespace epycbench::kernel {
namespace {
using CounterAccess = std::atomic_ref<std::uint64_t>;
static_assert(CounterAccess::is_always_lock_free);
static_assert(shared_atomic_counter_stride % CounterAccess::required_alignment == 0U);
static_assert(sizeof(std::uint64_t) < shared_atomic_counter_stride);

auto valid(AtomicCounterPattern pattern) noexcept -> bool {
    return pattern == AtomicCounterPattern::shared_slot ||
           pattern == AtomicCounterPattern::distinct_slots;
}

template <class Byte> auto first_padding(std::span<Byte> bytes) noexcept -> std::span<Byte> {
    return bytes.subspan(sizeof(std::uint64_t),
                         shared_atomic_counter_stride - sizeof(std::uint64_t));
}

template <class Byte> auto remaining_padding(std::span<Byte> bytes) noexcept -> std::span<Byte> {
    return bytes.subspan(shared_atomic_counter_stride + sizeof(std::uint64_t));
}

void reset_padding(std::span<std::byte> bytes) noexcept {
    std::ranges::fill(first_padding(bytes), shared_atomic_counter_padding);
    std::ranges::fill(remaining_padding(bytes), shared_atomic_counter_padding);
}
} // namespace

auto atomic_counter_work(TaskRepetitionCount count)
    -> std::expected<AtomicCounterWork, AtomicCounterError> {
    if (count.value() > std::numeric_limits<std::uint64_t>::max() / shared_atomic_counter_ranks)
        return std::unexpected{AtomicCounterError::increment_count_overflow};
    return AtomicCounterWork{
        count, core::WorkCount::from_base_units(count.value() * shared_atomic_counter_ranks)};
}

AtomicCounterRankOperation::AtomicCounterRankOperation(AtomicCounterRankOperation&& other) noexcept
    : destination_{std::exchange(other.destination_, nullptr)}, rank_{other.rank_},
      count_{other.count_} {}

auto AtomicCounterRankOperation::execute() && noexcept -> std::optional<AtomicCounterRankProgress> {
    auto* destination = std::exchange(destination_, nullptr);
    if (!destination)
        return std::nullopt;
    detail::increment_atomic_counter(*destination, count_.value());
    return AtomicCounterRankProgress{rank_, count_.value()};
}

auto AtomicCounterTrial::take_rank(std::size_t rank)
    -> std::expected<AtomicCounterRankOperation, AtomicCounterError> {
    if (rank >= pending_.size() || !pending_[rank].destination_)
        return std::unexpected{AtomicCounterError::rank_unavailable};
    return std::move(pending_[rank]);
}

auto AtomicCounterVerification::matched() const noexcept -> bool {
    const auto count = requested.team_tasks.value();
    if (!padding_intact || !valid(pattern) ||
        count > std::numeric_limits<std::uint64_t>::max() / shared_atomic_counter_ranks ||
        requested.increments.value() != count * shared_atomic_counter_ranks)
        return false;
    for (std::size_t rank{}; rank < ranks.size(); ++rank) {
        if (!ranks[rank] || ranks[rank]->rank != rank || ranks[rank]->completed_increments != count)
            return false;
    }
    const auto expected = pattern == AtomicCounterPattern::shared_slot
                              ? std::array{requested.increments.value(), std::uint64_t{}}
                              : std::array{count, count};
    return counters == expected;
}

auto AtomicCounterStorage::create(std::span<std::byte> bytes)
    -> std::expected<AtomicCounterStorage, AtomicCounterError> {
    if (bytes.size() < shared_atomic_counter_bytes)
        return std::unexpected{AtomicCounterError::storage_too_small};
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % shared_atomic_counter_stride != 0U)
        return std::unexpected{AtomicCounterError::unaligned_storage};

    reset_padding(bytes);
    auto* first =
        std::construct_at(reinterpret_cast<std::uint64_t*>(bytes.data()), std::uint64_t{});
    auto* second = std::construct_at(
        reinterpret_cast<std::uint64_t*>(bytes.data() + shared_atomic_counter_stride),
        std::uint64_t{});
    return AtomicCounterStorage{bytes, *first, *second};
}

AtomicCounterStorage::~AtomicCounterStorage() noexcept {
    if (!counters_[0])
        return;
    std::destroy_at(counters_[1]);
    std::destroy_at(counters_[0]);
}

AtomicCounterStorage::AtomicCounterStorage(AtomicCounterStorage&& other) noexcept
    : bytes_{std::exchange(other.bytes_, {})}, counters_{std::exchange(other.counters_, {})},
      generation_{std::exchange(other.generation_, 0U)} {}

auto AtomicCounterStorage::prepare(AtomicCounterPattern pattern, TaskRepetitionCount count)
    -> std::expected<AtomicCounterTrial, AtomicCounterError> {
    if (!counters_[0] || !valid(pattern) ||
        generation_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected{AtomicCounterError::invalid_trial};
    const auto work = atomic_counter_work(count);
    if (!work)
        return std::unexpected{work.error()};

    const auto [first, second] = counters_;
    reset_padding(bytes_);
    CounterAccess{*first}.store(0U, std::memory_order_relaxed);
    CounterAccess{*second}.store(0U, std::memory_order_relaxed);
    auto& second_destination = pattern == AtomicCounterPattern::shared_slot ? *first : *second;
    return AtomicCounterTrial{*first, second_destination, pattern, *work, ++generation_};
}

auto AtomicCounterStorage::owns(const AtomicCounterTrial& trial) const noexcept -> bool {
    return counters_[0] && trial.first_ == counters_[0] && trial.generation_ == generation_;
}

auto AtomicCounterStorage::verify(const AtomicCounterTrial& trial,
                                  AtomicCounterRanks ranks) const noexcept
    -> std::optional<AtomicCounterVerification> {
    if (!owns(trial))
        return std::nullopt;

    const auto is_padding = [](std::byte value) { return value == shared_atomic_counter_padding; };
    const std::span<const std::byte> bytes{bytes_};
    return AtomicCounterVerification{trial.pattern(),
                                     trial.work(),
                                     std::move(ranks),
                                     {CounterAccess{*counters_[0]}.load(std::memory_order_relaxed),
                                      CounterAccess{*counters_[1]}.load(std::memory_order_relaxed)},
                                     std::ranges::all_of(first_padding(bytes), is_padding) &&
                                         std::ranges::all_of(remaining_padding(bytes), is_padding)};
}

} // namespace epycbench::kernel
