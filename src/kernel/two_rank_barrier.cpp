#include "epycbench/kernel/two_rank_barrier.hpp"
#include "kernel/counter_trial_generation.hpp"
#include "kernel/package_cooperative_leaf.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

namespace epycbench::kernel {
namespace {
struct alignas(barrier_state_alignment) ControlWords {
    std::uint64_t arrivals{}, generation{}, cancelled{}, owner_generation{};
    std::array<std::byte, 32> padding{};
};
struct alignas(barrier_state_alignment) RankWords {
    std::uint64_t generation{};
    std::array<std::uint64_t, 2> witnesses{};
    std::array<std::byte, 40> padding{};
};
using Atomic = std::atomic_ref<std::uint64_t>;
static_assert(Atomic::is_always_lock_free && Atomic::required_alignment <= alignof(std::uint64_t));
}
struct alignas(barrier_state_alignment) BarrierState {
    ControlWords control;
    std::array<RankWords, 2> ranks;
};
static_assert(offsetof(BarrierState, ranks) == 64U && sizeof(RankWords) == 64U);
static_assert(sizeof(BarrierState) == barrier_state_bytes);
namespace {
auto valid(BarrierPattern pattern) noexcept -> bool {
    return pattern == BarrierPattern::central_generation || pattern == BarrierPattern::rank_flags;
}

auto wait_generation(Atomic generation, const std::uint64_t phase, Atomic cancelled, BarrierStopCheck stop) noexcept -> BarrierRankStatus {
    for (std::uint64_t poll{}; poll < barrier_poll_limit; ++poll) {
        if (generation.load(std::memory_order_acquire) >= phase) return BarrierRankStatus::complete;
        if (cancelled.load(std::memory_order_acquire) || ((poll & 255U) == 0U && stop.stopped()))
            return BarrierRankStatus::cancelled;
    }
    return BarrierRankStatus::peer_poll_limit;
}

template <BarrierPattern pattern>
auto run(BarrierState& state, std::size_t rank, std::uint64_t requested, BarrierStopCheck stop) noexcept -> BarrierRankProgress {
    auto& local = state.ranks[rank];
    const auto peer_rank = 1U - rank;
    auto& peer = state.ranks[peer_rank];
    auto& control = state.control;
    Atomic cancelled{control.cancelled};
    BarrierRankProgress progress{rank, 0U, 0U, 0U, BarrierRankStatus::complete};
    for (std::uint64_t phase{1U}; phase <= requested; ++phase) {
        if (cancelled.load(std::memory_order_acquire) || stop.stopped()) {
            cancelled.store(1U, std::memory_order_release);
            progress.status = BarrierRankStatus::cancelled;
            break;
        }
        const auto parity = static_cast<std::size_t>(phase & 1U);
        Atomic{local.witnesses[parity]}.store(2U * phase + rank, std::memory_order_relaxed);
        if constexpr (pattern == BarrierPattern::central_generation) {
            Atomic arrivals{control.arrivals};
            if (arrivals.fetch_add(1U, std::memory_order_acq_rel) == 1U) {
                arrivals.store(0U, std::memory_order_relaxed);
                Atomic{control.generation}.store(phase, std::memory_order_release);
            } else {
                progress.status = wait_generation(Atomic{control.generation}, phase, cancelled, stop);
            }
        } else {
            Atomic{local.generation}.store(phase, std::memory_order_release);
            progress.status = wait_generation(Atomic{peer.generation}, phase, cancelled, stop);
        }
        if (progress.status != BarrierRankStatus::complete) {
            cancelled.store(1U, std::memory_order_release);
            break;
        }
        const auto witness = Atomic{peer.witnesses[parity]}.load(std::memory_order_relaxed);
        if (witness != 2U * phase + peer_rank) {
            progress.status = BarrierRankStatus::witness_mismatch;
            cancelled.store(1U, std::memory_order_release);
            break;
        }
        ++progress.completed_rounds;
        ++progress.checked_peer_tokens;
        progress.peer_token_sum += witness;
    }
    return progress;
}

auto token_for_parity(std::uint64_t count, std::size_t rank, std::size_t parity) noexcept -> std::uint64_t {
    const auto phase = count - ((count & 1U) != parity);
    return phase ? 2U * phase + rank : 0U;
}
}

void detail::initialize_packaged_barrier(unsigned char* storage) noexcept {
    auto* state = std::construct_at(reinterpret_cast<BarrierState*>(storage));
    state->control.owner_generation = 1U;
    state->control.padding.fill(barrier_padding);
    for (auto& rank : state->ranks) rank.padding.fill(barrier_padding);
}

auto detail::run_packaged_barrier_rank(unsigned char* storage, std::size_t rank, std::uint64_t repetitions,
                                       BarrierPattern pattern, BarrierStopCheck stop) noexcept -> BarrierRankProgress {
    auto& state = *reinterpret_cast<BarrierState*>(storage);
    return pattern == BarrierPattern::central_generation
        ? run<BarrierPattern::central_generation>(state, rank, repetitions, stop)
        : run<BarrierPattern::rank_flags>(state, rank, repetitions, stop);
}

BarrierRankOperation::BarrierRankOperation(BarrierRankOperation&& other) noexcept
    : state_{std::exchange(other.state_, nullptr)}, rank_{other.rank_}, pattern_{other.pattern_},
      rounds_{other.rounds_}, owner_generation_{other.owner_generation_} {}

auto BarrierRankOperation::execute(BarrierStopCheck stop) && noexcept -> std::optional<BarrierRankProgress> {
    auto* state = std::exchange(state_, nullptr);
    if (!state || state->control.owner_generation != owner_generation_) return std::nullopt;
    return pattern_ == BarrierPattern::central_generation ? run<BarrierPattern::central_generation>(*state, rank_, rounds_.value(), stop)
                                                        : run<BarrierPattern::rank_flags>(*state, rank_, rounds_.value(), stop);
}
BarrierTrial::BarrierTrial(BarrierTrial&& other) noexcept
    : state_{std::exchange(other.state_, nullptr)}, pattern_{other.pattern_}, rounds_{other.rounds_},
      generation_{other.generation_}, pending_{std::move(other.pending_)} {}

auto BarrierTrial::take_rank(std::size_t rank) -> std::expected<BarrierRankOperation, AtomicCounterError> {
    if (rank >= pending_.size() || !pending_[rank].state_) return std::unexpected{AtomicCounterError::invalid_trial};
    return std::move(pending_[rank]);
}
auto BarrierVerification::matched() const noexcept -> bool {
    const auto count = rounds.value();
    if (!valid(pattern) || count > UINT64_MAX / 2U || !padding_intact || cancelled || arrivals) return false;
    const bool central = pattern == BarrierPattern::central_generation;
    if (generation != (central ? count : 0U)) return false;
    for (std::size_t rank{}; rank < ranks.size(); ++rank) {
        const auto& receipt = ranks[rank];
        const auto peer_rank = 1U - rank;
        if (!receipt || receipt->rank != rank || receipt->status != BarrierRankStatus::complete ||
            receipt->completed_rounds != count || receipt->checked_peer_tokens != count ||
            receipt->peer_token_sum != count * (count + 1U) + count * peer_rank ||
            published_generations[rank] != (central ? 0U : count)) return false;
        for (std::size_t parity{}; parity < 2U; ++parity)
            if (witnesses[rank][parity] != token_for_parity(count, rank, parity)) return false;
    }
    return true;
}

auto BarrierStorage::create(std::span<std::byte> bytes) -> std::expected<BarrierStorage, AtomicCounterError> {
    if (bytes.size() < barrier_state_bytes) return std::unexpected{AtomicCounterError::storage_too_small};
    if (reinterpret_cast<std::uintptr_t>(bytes.data()) % barrier_state_alignment)
        return std::unexpected{AtomicCounterError::unaligned_storage};
    return BarrierStorage{bytes, std::construct_at(reinterpret_cast<BarrierState*>(bytes.data()))};
}
BarrierStorage::BarrierStorage(BarrierStorage&& other) noexcept
    : bytes_{std::exchange(other.bytes_, {})}, state_{std::exchange(other.state_, nullptr)} {}
BarrierStorage::~BarrierStorage() noexcept {
    if (state_) std::destroy_at(state_);
}
auto BarrierStorage::prepare(BarrierPattern pattern, TaskRepetitionCount count)
    -> std::expected<BarrierTrial, AtomicCounterError> {
    if (!state_ || !valid(pattern) || count.value() > UINT64_MAX / 2U)
        return std::unexpected{AtomicCounterError::invalid_trial};
    const auto generation = detail::next_counter_trial_generation();
    if (!generation) return std::unexpected{AtomicCounterError::invalid_trial};
    *state_ = {};
    state_->control.owner_generation = *generation;
    state_->control.padding.fill(barrier_padding);
    for (auto& rank : state_->ranks) rank.padding.fill(barrier_padding);
    std::ranges::fill(bytes_.subspan(barrier_state_bytes), barrier_padding);
    return BarrierTrial{state_, pattern, count, *generation};
}
auto BarrierStorage::owns(const BarrierTrial& trial) const noexcept -> bool {
    return state_ && state_ == trial.state_ && state_->control.owner_generation == trial.generation_;
}
void BarrierStorage::cancel(const BarrierTrial& trial) noexcept {
    if (owns(trial)) Atomic{state_->control.cancelled}.store(1U, std::memory_order_release);
}
auto BarrierStorage::verify(const BarrierTrial& trial, BarrierRanks ranks) const noexcept -> std::optional<BarrierVerification> {
    if (!owns(trial)) return std::nullopt;
    const auto& a = state_->ranks[0];
    const auto& b = state_->ranks[1];
    const auto& control = state_->control;
    const auto clean = [](auto bytes) {
        return std::ranges::all_of(bytes, [](std::byte byte) { return byte == barrier_padding; });
    };
    return BarrierVerification{trial.pattern(), trial.work(), std::move(ranks), control.arrivals, control.generation,
        control.cancelled, {a.generation, b.generation}, {a.witnesses, b.witnesses},
        clean(std::span{control.padding}) && clean(std::span{a.padding}) && clean(std::span{b.padding}) &&
        clean(bytes_.subspan(barrier_state_bytes))};
}
} // namespace epycbench::kernel
