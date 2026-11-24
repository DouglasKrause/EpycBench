#include "collective_sum_support.hpp"

#include "../output_task_module_support.hpp"

extern "C" auto epycbench_kernel_execute_v2(const EpycBenchKernelRequestV2*, std::uint64_t*) noexcept -> int;

namespace {
using namespace epycbench::bundled_packages::collective_sum;
using AtomicWord = std::atomic_ref<std::uint64_t>;
static_assert(AtomicWord::is_always_lock_free);

auto alternative(const EpycBenchPackageCallV1& call) noexcept -> std::optional<bool> {
    if (call.tuning_count != 1 || !call.tuning || call.tuning[0].type != EPYCBENCH_PACKAGE_ENUM) return std::nullopt;
    const auto& item = call.tuning[0];
    if (!item.name.data || item.name.bytes != 11 || std::string_view{item.name.data, 11} != "aggregation" ||
        !item.value.enum_value.data || item.value.enum_value.bytes > 13)
        return std::nullopt;
    const auto name = std::string_view{item.value.enum_value.data, static_cast<std::size_t>(item.value.enum_value.bytes)};
    if (name == "flat") return false;
    if (name == "topology-tree") return true;
    return std::nullopt;
}

auto stop(Control& control, EpycBenchPackageCancellationV1 cancellation) noexcept -> bool {
    const AtomicWord stopped{control.cancelled};
    if (stopped.load(std::memory_order_acquire)) return true;
    if (!cancellation.requested || cancellation.requested(cancellation.context) == 0) return false;
    stopped.store(1, std::memory_order_release);
    return true;
}

auto rendezvous(Control& control, std::uint32_t ranks, std::uint64_t& phase,
                EpycBenchPackageCancellationV1 cancellation) noexcept -> bool {
    if (stop(control, cancellation)) return false;
    const AtomicWord arrivals{control.arrivals}, epoch{control.epoch};
    const auto departed = ++phase;
    if (arrivals.fetch_add(1, std::memory_order_acq_rel) == ranks - 1U) {
        arrivals.store(0, std::memory_order_relaxed);
        epoch.store(departed, std::memory_order_release);
        return true;
    }
    constexpr std::uint64_t maximum_polls = UINT64_C(1) << 26U;
    for (std::uint64_t poll = 0; poll < maximum_polls; ++poll) {
        if (epoch.load(std::memory_order_acquire) == departed) return true;
        if ((poll & 255U) == 0U && stop(control, cancellation)) return false;
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
    AtomicWord{control.cancelled}.store(1, std::memory_order_release);
    return false;
}

template <bool hierarchical>
auto execute_repetitions(const EpycBenchPackageCallV1& call, const Schedule& plan,
                         unsigned char* state, const unsigned char* schedule,
                         EpycBenchPackageWorkV1& work) noexcept -> int {
    auto& control = *reinterpret_cast<Control*>(state);
    const auto ranks = call.task->rank_count;
    const auto rank = call.task->rank;
    const auto repetitions = call.task->repetitions;
    const EpycBenchKernelRequestV2 local{call.inputs[0].data, call.inputs[0].bytes, 1};
    std::uint64_t phase{}, folded{}, completed{};
    int status = 0;
    for (; completed < repetitions;) {
        if (stop(control, call.cancellation)) break;
        std::uint64_t partial{};
        status = epycbench_kernel_execute_v2(&local, &partial);
        if (status != 0) {
            AtomicWord{control.cancelled}.store(1, std::memory_order_release);
            break;
        }
        ++completed;
        folded += partial;
        store_partial(state, rank, partial);
        if (!rendezvous(control, ranks, phase, call.cancellation)) { status = 1; break; }
        if constexpr (hierarchical) {
            for (std::uint32_t stage = 0; stage < plan.stages; ++stage) {
                Merge merge;
                std::memcpy(&merge, merge_bytes(schedule, stage * ranks + rank), sizeof(merge));
                if (merge.source_count != 0) {
                    std::uint64_t sum{};
                    for (std::uint32_t source = 0; source < merge.source_count; ++source) {
                        std::uint32_t slot{};
                        std::memcpy(&slot, source_bytes(schedule, ranks, merge.first_source + source), sizeof(slot));
                        sum += load_partial(state, slot);
                    }
                    store_partial(state, merge.destination, sum);
                }
                if (stage + 1U < plan.stages && !rendezvous(control, ranks, phase, call.cancellation)) {
                    status = 1;
                    break;
                }
            }
            if (status != 0) break;
        }
        if (rank == 0) {
            std::uint64_t sum{};
            if constexpr (hierarchical) sum = load_partial(state, plan.result_slot);
            else for (std::uint32_t source = 0; source < ranks; ++source) sum += load_partial(state, source);
            control.checksum += sum;
            ++control.combined;
        }
        if (!rendezvous(control, ranks, phase, call.cancellation)) { status = 1; break; }
    }
    epycbench::module_detail::store_xor_word(call.outputs[0].data, 0, folded);
    work = {completed, call.task->partition_count * completed, 1};
    return status == 0 && completed != repetitions ? 1 : status;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_package_execute_collective_sum_v1(const EpycBenchPackageCallV1* call,
                                                EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!call || !work || !valid_team(call->task) || call->input_count != 1 || call->output_count != 1 ||
        !call->inputs || !call->outputs || !call->outputs[0].data || call->outputs[0].bytes != sizeof(std::uint64_t) ||
        !call->shared || call->shared_count != 1 || !call->shared_inputs || call->shared_input_count != 1 ||
        !call->shared[0].data || call->shared[0].bytes != state_bytes(call->task->rank_count) ||
        !call->shared_inputs[0].data || call->shared_inputs[0].bytes != schedule_bytes(call->task->rank_count) ||
        call->task->partition_count == 0 || call->task->partition_count > UINT64_MAX / sizeof(std::uint64_t) ||
        call->inputs[0].bytes != call->task->partition_count * sizeof(std::uint64_t))
        return 1;
    const auto hierarchical = alternative(*call);
    if (!hierarchical) return 1;
    auto* state = call->shared[0].data;
    const auto* schedule = call->shared_inputs[0].data;
    const auto& plan = *reinterpret_cast<const Schedule*>(schedule);
    if (plan.ranks != call->task->rank_count || (plan.stages == 0 && plan.ranks > 1U) || plan.stages > maximum_stages ||
        plan.result_slot >= 2U * plan.ranks - 1U)
        return 1;
    return *hierarchical ? execute_repetitions<true>(*call, plan, state, schedule, *work)
                         : execute_repetitions<false>(*call, plan, state, schedule, *work);
}

extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageImplementationV1),
        epycbench_package_execute_collective_sum_v1, nullptr};
    return &descriptor;
}
