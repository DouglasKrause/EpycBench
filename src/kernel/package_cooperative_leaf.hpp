#pragma once

#include "epycbench/kernel/lock_counter.hpp"
#include "epycbench/kernel/spsc_queue.hpp"
#include "epycbench/kernel/two_rank_barrier.hpp"

namespace epycbench::kernel::detail {

auto run_packaged_lock_rank(unsigned char* storage, std::size_t rank, std::uint64_t repetitions,
                            LockCounterPattern, BarrierStopCheck) noexcept -> std::uint64_t;
void initialize_packaged_barrier(unsigned char* storage) noexcept;
auto run_packaged_barrier_rank(unsigned char* storage, std::size_t rank, std::uint64_t repetitions,
                               BarrierPattern, BarrierStopCheck) noexcept -> BarrierRankProgress;
void initialize_packaged_queue(unsigned char* storage) noexcept;
auto run_packaged_queue_rank(unsigned char* storage, std::size_t rank, std::uint64_t repetitions,
                             QueuePattern, QueueStopCheck) noexcept -> QueueRankProgress;

} // namespace epycbench::kernel::detail
