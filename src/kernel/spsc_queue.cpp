#include "epycbench/kernel/spsc_queue.hpp"
#include "kernel/counter_trial_generation.hpp"
#include "kernel/package_cooperative_leaf.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace epycbench::kernel {
namespace {
struct alignas(queue_state_alignment) Control {
    std::uint64_t generation{}, cancelled{};
    std::array<std::byte, 48> padding{};
};
struct alignas(queue_state_alignment) Cursor {
    std::uint64_t value{};
    std::array<std::byte, 56> padding{};
};
using Atomic = std::atomic_ref<std::uint64_t>;
static_assert(Atomic::is_always_lock_free && Atomic::required_alignment <= alignof(std::uint64_t));
}

struct alignas(queue_state_alignment) QueueState {
    Control control;
    Cursor published, consumed;
    std::array<std::uint64_t, queue_slots> slots{};
};
static_assert(offsetof(QueueState, published) == 64U && offsetof(QueueState, consumed) == 128U &&
              offsetof(QueueState, slots) == 192U && sizeof(QueueState) == queue_state_bytes);

namespace {
auto valid(QueuePattern pattern) noexcept -> bool {
    return pattern == QueuePattern::direct_cursor || pattern == QueuePattern::cached_cursor;
}

auto prefix_sum(std::uint64_t count) noexcept -> std::uint64_t {
    return count % 2U ? count * (count / 2U + 1U) : (count / 2U) * (count + 1U);
}

// An old peer cursor is conservative on both sides. Only an acquire load may
// extend the part of the ring that this rank can now read or overwrite.
template<bool Producer, QueuePattern Pattern>
auto transfer(QueueState& state, std::uint64_t items, QueueStopCheck stop) noexcept -> QueueRankProgress {
    Atomic own{Producer ? state.published.value : state.consumed.value};
    Atomic peer{Producer ? state.consumed.value : state.published.value};
    Atomic cancelled{state.control.cancelled};
    QueueRankProgress progress{Producer ? 0U : 1U, 0U, 0U, 0U, QueueRankStatus::complete};
    std::uint64_t peer_cursor{};
    while (progress.transfers < items) {
        const auto cursor = progress.transfers;
        const auto available = [&] {
            if constexpr (Producer) return cursor - peer_cursor < queue_slots;
            else return peer_cursor > cursor;
        };
        if (stop.stopped() || cancelled.load(std::memory_order_acquire)) {
            progress.status = QueueRankStatus::cancelled;
            break;
        }
        if (Pattern == QueuePattern::direct_cursor || !available()) {
            std::uint64_t polls{};
            while (polls < queue_poll_limit) {
                peer_cursor = peer.load(std::memory_order_acquire);
                if (available()) break;
                ++polls;
                if (stop.stopped() || cancelled.load(std::memory_order_acquire)) break;
            }
            if (!available()) {
                progress.status = (stop.stopped() || cancelled.load(std::memory_order_acquire))
                    ? QueueRankStatus::cancelled : QueueRankStatus::peer_poll_limit;
                break;
            }
        }
        auto& slot = state.slots[cursor % queue_slots];
        const auto expected = cursor + 1U;
        if constexpr (Producer) {
            slot = expected;
            progress.token_sum += expected;
        } else {
            const auto actual = slot;
            if (actual != expected) {
                progress.status = QueueRankStatus::token_mismatch;
                break;
            }
            ++progress.checked_tokens;
            progress.token_sum += actual;
        }
        own.store(expected, std::memory_order_release);
        progress.transfers = expected;
    }
    if (progress.status != QueueRankStatus::complete) cancelled.store(1U, std::memory_order_release);
    return progress;
}
}

void detail::initialize_packaged_queue(unsigned char* storage) noexcept {
    auto* state = std::construct_at(reinterpret_cast<QueueState*>(storage));
    state->control.generation = 1U;
    state->control.padding.fill(queue_padding);
    state->published.padding.fill(queue_padding);
    state->consumed.padding.fill(queue_padding);
}

auto detail::run_packaged_queue_rank(unsigned char* storage, std::size_t rank, std::uint64_t repetitions,
                                     QueuePattern pattern, QueueStopCheck stop) noexcept -> QueueRankProgress {
    auto& state = *reinterpret_cast<QueueState*>(storage);
    const bool direct = pattern == QueuePattern::direct_cursor;
    if (rank == 0U)
        return direct ? transfer<true, QueuePattern::direct_cursor>(state, repetitions, stop)
                      : transfer<true, QueuePattern::cached_cursor>(state, repetitions, stop);
    return direct ? transfer<false, QueuePattern::direct_cursor>(state, repetitions, stop)
                  : transfer<false, QueuePattern::cached_cursor>(state, repetitions, stop);
}

QueueRankOperation::QueueRankOperation(QueueRankOperation&& other) noexcept
    : state_{std::exchange(other.state_, nullptr)}, rank_{other.rank_}, pattern_{other.pattern_},
      items_{other.items_}, owner_generation_{other.owner_generation_} {}

auto QueueRankOperation::execute(std::stop_token stop) && noexcept -> std::optional<QueueRankProgress> {
    return std::move(*this).execute(QueueStopCheck{&stop, [](const void* context) noexcept {
        return static_cast<const std::stop_token*>(context)->stop_requested();
    }});
}
auto QueueRankOperation::execute(QueueStopCheck stop) && noexcept -> std::optional<QueueRankProgress> {
    auto* state = std::exchange(state_, nullptr);
    if (!state || state->control.generation != owner_generation_) return std::nullopt;
    const bool direct = pattern_ == QueuePattern::direct_cursor;
    if (rank_ == 0U)
        return direct ? transfer<true, QueuePattern::direct_cursor>(*state, items_.value(), stop)
                      : transfer<true, QueuePattern::cached_cursor>(*state, items_.value(), stop);
    return direct ? transfer<false, QueuePattern::direct_cursor>(*state, items_.value(), stop)
                  : transfer<false, QueuePattern::cached_cursor>(*state, items_.value(), stop);
}
QueueTrial::QueueTrial(QueueTrial&& other) noexcept
    : state_{std::exchange(other.state_, nullptr)}, pattern_{other.pattern_}, items_{other.items_},
      generation_{other.generation_}, pending_{std::move(other.pending_)} {}

auto QueueTrial::take_rank(std::size_t rank) -> std::expected<QueueRankOperation, AtomicCounterError> {
    if (rank >= pending_.size() || !pending_[rank].state_) return std::unexpected{AtomicCounterError::invalid_trial};
    return std::move(pending_[rank]);
}
auto QueueVerification::matched() const noexcept -> bool {
    const auto count = items.value();
    if (!valid(pattern) || !padding_intact || cancelled || published != count || consumed != count) return false;
    for (std::size_t rank{}; rank < ranks.size(); ++rank) {
        const auto& receipt = ranks[rank];
        if (!receipt || receipt->rank != rank || receipt->status != QueueRankStatus::complete ||
            receipt->transfers != count || receipt->checked_tokens != (rank == 1U ? count : 0U) ||
            receipt->token_sum != prefix_sum(count)) return false;
    }
    for (std::size_t slot{}; slot < slots.size(); ++slot) {
        const auto first = slot + 1U;
        const auto last = count < first ? 0U : first + (count - first) / queue_slots * queue_slots;
        if (slots[slot] != last) return false;
    }
    return true;
}

auto QueueStorage::create(std::span<std::byte> bytes) -> std::expected<QueueStorage, AtomicCounterError> {
    if (bytes.size() < queue_state_bytes) return std::unexpected{AtomicCounterError::storage_too_small};
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % queue_state_alignment)
        return std::unexpected{AtomicCounterError::unaligned_storage};
    return QueueStorage{bytes, std::construct_at(reinterpret_cast<QueueState*>(bytes.data()))};
}
QueueStorage::QueueStorage(QueueStorage&& other) noexcept
    : bytes_{std::exchange(other.bytes_, {})}, state_{std::exchange(other.state_, nullptr)} {}
QueueStorage::~QueueStorage() noexcept {
    if (state_) std::destroy_at(state_);
}
auto QueueStorage::prepare(QueuePattern pattern, TaskRepetitionCount count)
    -> std::expected<QueueTrial, AtomicCounterError> {
    if (!state_ || !valid(pattern))
        return std::unexpected{AtomicCounterError::invalid_trial};
    const auto generation = detail::next_counter_trial_generation();
    if (!generation) return std::unexpected{AtomicCounterError::invalid_trial};
    *state_ = {};
    state_->control.generation = *generation;
    state_->control.padding.fill(queue_padding);
    state_->published.padding.fill(queue_padding);
    state_->consumed.padding.fill(queue_padding);
    std::ranges::fill(bytes_.subspan(queue_state_bytes), queue_padding);
    return QueueTrial{state_, pattern, count, *generation};
}
auto QueueStorage::owns(const QueueTrial& trial) const noexcept -> bool {
    return state_ && state_ == trial.state_ && state_->control.generation == trial.generation_;
}
void QueueStorage::cancel(const QueueTrial& trial) noexcept {
    if (owns(trial)) Atomic{state_->control.cancelled}.store(1U, std::memory_order_release);
}
auto QueueStorage::verify(const QueueTrial& trial, QueueRanks ranks) const noexcept -> std::optional<QueueVerification> {
    if (!owns(trial)) return std::nullopt;
    const auto clean = [](auto bytes) {
        return std::ranges::all_of(bytes, [](std::byte byte) { return byte == queue_padding; });
    };
    return QueueVerification{trial.pattern(), trial.work(), std::move(ranks), state_->published.value,
        state_->consumed.value, state_->control.cancelled, state_->slots, clean(std::span{state_->control.padding}) &&
        clean(std::span{state_->published.padding}) && clean(std::span{state_->consumed.padding}) &&
        clean(bytes_.subspan(queue_state_bytes))};
}
} // namespace epycbench::kernel
