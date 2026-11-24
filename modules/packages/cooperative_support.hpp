#pragma once

#include "epycbench/kernel/false_sharing_counter.hpp"
#include "epycbench/kernel/package.h"
#include "kernel/atomic_counter_operation.hpp"
#include "kernel/package_cooperative_leaf.hpp"

#include <algorithm>
#include <cstring>

namespace epycbench::bundled_packages {

enum class CooperativeTask { atomic_counter, false_sharing, lock_counter, barrier, queue };
inline constexpr auto task = CooperativeTask::EPYCBENCH_BUNDLED_COOPERATIVE;
inline constexpr bool rank_receipt = task == CooperativeTask::barrier || task == CooperativeTask::queue;
inline constexpr bool variants_change_state = task == CooperativeTask::atomic_counter ||
    task == CooperativeTask::false_sharing || task == CooperativeTask::barrier;
inline constexpr std::size_t rank_receipt_bytes = 2U * sizeof(std::uint64_t);

consteval auto shared_bytes_for_task() -> std::size_t {
    using namespace kernel;
    switch (task) {
    case CooperativeTask::atomic_counter: return shared_atomic_counter_bytes;
    case CooperativeTask::false_sharing: return false_sharing_counter_bytes;
    case CooperativeTask::lock_counter: return lock_counter_bytes;
    case CooperativeTask::barrier: return barrier_state_bytes;
    case CooperativeTask::queue: return queue_state_bytes;
    }
    return 0;
}
inline constexpr auto shared_bytes = shared_bytes_for_task();
inline constexpr auto shared_alignment = 64U;

inline auto valid_request(const EpycBenchPackageTaskRequestV1* request) noexcept -> bool {
    return request && request->rank_count == 2 && request->rank < 2 && request->repetitions > 0 &&
        request->repetitions <= UINT64_MAX / 2U && request->partition_begin == 0 &&
        request->partition_count == 0;
}

template <typename Region>
auto valid_shared(const Region* regions, std::uint32_t count) noexcept -> bool {
    return count == 1U && regions && regions[0].data && regions[0].bytes == shared_bytes &&
        reinterpret_cast<std::uintptr_t>(regions[0].data) % shared_alignment == 0;
}

inline void store_word(unsigned char* data, std::size_t offset, std::uint64_t value) noexcept {
    std::memcpy(data + offset, &value, sizeof(value));
}

inline auto rank_work(std::uint64_t completed) noexcept -> EpycBenchPackageWorkV1 {
    return {completed, completed, rank_receipt ? 2U : 0U};
}

inline auto team_work(std::uint64_t first, std::uint64_t second) noexcept -> EpycBenchPackageWorkV1 {
    const auto whole = std::min(first, second);
    return {whole, rank_receipt ? whole : first + second, 0};
}

inline auto expected_prefix_sum(std::uint64_t count) noexcept -> std::uint64_t {
    return count % 2U == 0 ? (count / 2U) * (count + 1U) : count * (count / 2U + 1U);
}

} // namespace epycbench::bundled_packages
