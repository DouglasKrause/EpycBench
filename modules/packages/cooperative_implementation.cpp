#include "cooperative_support.hpp"

namespace {
using namespace epycbench::bundled_packages;
namespace kernel = epycbench::kernel;

constexpr bool alternate = EPYCBENCH_BUNDLED_ALTERNATE;
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_package_execute_cooperative_v1(const EpycBenchPackageCallV1* call,
                                             EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!call || !work || !valid_request(call->task) || !valid_shared(call->shared, call->shared_count))
        return 1;
    const auto rank = static_cast<std::size_t>(call->task->rank);
    const auto repetitions = call->task->repetitions;
    auto* shared = call->shared[0].data;
    const auto stop_callback = [](const void* context) noexcept -> bool {
        const auto& cancellation = *static_cast<const EpycBenchPackageCancellationV1*>(context);
        return cancellation.requested && cancellation.requested(cancellation.context) != 0;
    };
    const kernel::BarrierStopCheck stop{&call->cancellation, stop_callback};
    if constexpr (task == CooperativeTask::atomic_counter || task == CooperativeTask::false_sharing) {
        if (stop.stopped()) return 1;
        constexpr auto rank_one_offset = task == CooperativeTask::atomic_counter
            ? (alternate ? 64U : 0U) : (alternate ? 64U : 8U);
        auto& word = *reinterpret_cast<std::uint64_t*>(shared + rank * rank_one_offset);
        kernel::detail::increment_atomic_counter(word, repetitions);
        *work = rank_work(repetitions);
        return 0;
    } else if constexpr (task == CooperativeTask::lock_counter) {
        constexpr auto pattern = alternate ? kernel::LockCounterPattern::test_test_and_set
                                           : kernel::LockCounterPattern::test_and_set;
        const auto completed = kernel::detail::run_packaged_lock_rank(shared, rank, repetitions, pattern, stop);
        *work = rank_work(completed);
        return completed == repetitions ? 0 : 1;
    } else {
        if (call->output_count != 1 || !call->outputs || !call->outputs[0].data ||
            call->outputs[0].bytes != rank_receipt_bytes)
            return 1;
        auto* receipt = call->outputs[0].data;
        if constexpr (task == CooperativeTask::barrier) {
            constexpr auto pattern = alternate ? kernel::BarrierPattern::rank_flags
                                               : kernel::BarrierPattern::central_generation;
            const auto progress = kernel::detail::run_packaged_barrier_rank(shared, rank, repetitions, pattern, stop);
            store_word(receipt, 0, progress.checked_peer_tokens);
            store_word(receipt, 8, progress.peer_token_sum);
            *work = rank_work(progress.completed_rounds);
            return static_cast<int>(progress.status);
        } else {
            constexpr auto pattern = alternate ? kernel::QueuePattern::cached_cursor : kernel::QueuePattern::direct_cursor;
            const auto progress = kernel::detail::run_packaged_queue_rank(shared, rank, repetitions, pattern,
                kernel::QueueStopCheck{&call->cancellation, stop_callback});
            store_word(receipt, 0, progress.checked_tokens);
            store_word(receipt, 8, progress.token_sum);
            *work = rank_work(progress.transfers);
            return static_cast<int>(progress.status);
        }
    }
}
extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageImplementationV1),
        epycbench_package_execute_cooperative_v1};
    return &descriptor;
}
