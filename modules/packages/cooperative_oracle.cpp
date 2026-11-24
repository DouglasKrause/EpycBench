#include "cooperative_support.hpp"
#include "task_u64_support.hpp"

#include <array>
#include <memory>

namespace {
using namespace epycbench::bundled_packages;
namespace kernel = epycbench::kernel;

auto rank_layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* layout) noexcept -> int {
    if (!valid_request(request) || !layout) return 1;
    layout->partition_begin = 0;
    layout->partition_count = 0;
    layout->region_count = rank_receipt ? 1U : 0U;
    if constexpr (rank_receipt)
        layout->regions[0] = {"receipt", EPYCBENCH_PACKAGE_OUTPUT, rank_receipt_bytes, shared_alignment};
    return 0;
}

auto rank_initialize(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageWriteRegionV1*,
                     std::uint32_t count) noexcept -> int {
    return valid_request(request) && count == 0 ? 0 : 1;
}

auto rank_reference(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1*,
                    std::uint32_t input_count, const EpycBenchPackageWriteRegionV1* outputs,
                    std::uint32_t output_count, EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!valid_request(request) || !work || input_count != 0 || output_count != (rank_receipt ? 1U : 0U))
        return 1;
    const auto repetitions = request->repetitions;
    if constexpr (rank_receipt) {
        if (!outputs || !outputs[0].data || outputs[0].bytes != rank_receipt_bytes) return 1;
        const auto checks = (task == CooperativeTask::barrier || request->rank == 1) ? repetitions : 0U;
        const auto sum = task == CooperativeTask::barrier
            ? repetitions * (repetitions + 1U + 1U - request->rank) : expected_prefix_sum(repetitions);
        store_word(outputs[0].data, 0, checks);
        store_word(outputs[0].data, 8, sum);
    }
    *work = rank_work(repetitions);
    return 0;
}

auto rank_compare(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1* observed,
                  const EpycBenchPackageReadRegionV1* expected, std::uint32_t count) noexcept -> int {
    if constexpr (rank_receipt) return count == 1U ? compare_outputs(observed, expected, count) : 1;
    return count == 0 ? 0 : 1;
}

auto team_layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* layout) noexcept -> int {
    if (!valid_request(request) || !layout) return 1;
    layout->partition_begin = 0;
    layout->partition_count = 0;
    layout->region_count = 1;
    layout->regions[0] = {"shared_state", EPYCBENCH_PACKAGE_SHARED, shared_bytes, shared_alignment};
    return 0;
}

template <std::size_t Count>
void initialize_words(unsigned char* data, const std::array<std::size_t, Count>& offsets) noexcept {
    std::fill_n(data, shared_bytes, 0xa5);
    for (const auto offset : offsets)
        std::construct_at(reinterpret_cast<std::uint64_t*>(data + offset), std::uint64_t{});
}

auto team_initialize(const EpycBenchPackageTaskRequestV1* request,
                     const EpycBenchPackageWriteRegionV1* regions, std::uint32_t count) noexcept -> int {
    if (!valid_request(request) || !valid_shared(regions, count)) return 1;
    auto* data = regions[0].data;
    if constexpr (task == CooperativeTask::atomic_counter)
        initialize_words(data, std::array<std::size_t, 2>{0, kernel::shared_atomic_counter_stride});
    else if constexpr (task == CooperativeTask::false_sharing)
        initialize_words(data, kernel::false_sharing_counter_offsets);
    else if constexpr (task == CooperativeTask::lock_counter)
        initialize_words(data, kernel::lock_counter_offsets);
    else if constexpr (task == CooperativeTask::barrier)
        kernel::detail::initialize_packaged_barrier(data);
    else
        kernel::detail::initialize_packaged_queue(data);
    return 0;
}

void expected_terminal(unsigned char* data, std::uint64_t repetitions, bool alternate) noexcept {
    std::fill_n(data, shared_bytes, 0xa5);
    if constexpr (task == CooperativeTask::atomic_counter) {
        store_word(data, 0, alternate ? repetitions : repetitions * 2U);
        store_word(data, 64, alternate ? repetitions : 0U);
    } else if constexpr (task == CooperativeTask::false_sharing) {
        store_word(data, 0, repetitions);
        store_word(data, 8, alternate ? 0U : repetitions);
        store_word(data, 64, alternate ? repetitions : 0U);
    } else if constexpr (task == CooperativeTask::lock_counter) {
        store_word(data, kernel::lock_counter_offsets[0], 0);
        store_word(data, kernel::lock_counter_offsets[1], repetitions * 2U);
        store_word(data, kernel::lock_counter_offsets[2], repetitions);
        store_word(data, kernel::lock_counter_offsets[3], repetitions);
    } else if constexpr (task == CooperativeTask::barrier) {
        store_word(data, 0, 0);
        store_word(data, 8, alternate ? 0U : repetitions);
        store_word(data, 16, 0);
        store_word(data, 24, 1U);
        for (std::size_t rank = 0; rank < 2U; ++rank) {
            const auto line = 64U * (rank + 1U);
            store_word(data, line, alternate ? repetitions : 0U);
            for (std::size_t parity = 0; parity < 2U; ++parity) {
                const auto phase = repetitions - ((repetitions & 1U) != parity ? 1U : 0U);
                store_word(data, line + 8U * (parity + 1U), phase == 0 ? 0U : 2U * phase + rank);
            }
        }
    } else {
        store_word(data, 0, 1U);
        store_word(data, 8, 0);
        store_word(data, 64, repetitions);
        store_word(data, 128, repetitions);
        for (std::size_t slot = 0; slot < kernel::queue_slots; ++slot) {
            const auto first = slot + 1U;
            const auto last = repetitions < first ? 0U : first + (repetitions - first) / kernel::queue_slots * kernel::queue_slots;
            store_word(data, 192U + 8U * slot, last);
        }
    }
}

auto team_reference(const EpycBenchPackageTaskRequestV1* request,
                    const EpycBenchPackageCollectiveInputV1* ranks, std::uint32_t rank_count,
                    const EpycBenchPackageWriteRegionV1* regions, std::uint32_t region_count,
                    EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!valid_request(request) || !work || !valid_shared(regions, region_count) || !ranks || rank_count != 2U)
        return 1;
    const auto repetitions = request->repetitions;
    for (std::uint32_t rank = 0; rank < rank_count; ++rank) {
        if (!valid_request(ranks[rank].task) || ranks[rank].task->rank != rank ||
            ranks[rank].task->repetitions != repetitions || ranks[rank].input_count != 0)
            return 1;
    }
    expected_terminal(regions[0].data, repetitions, false);
    *work = team_work(repetitions, repetitions);
    return 0;
}

auto team_compare(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* observed,
                  const EpycBenchPackageReadRegionV1* expected, std::uint32_t region_count,
                  const EpycBenchPackageRankReceiptV1* receipts, std::uint32_t rank_count,
                  EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!valid_request(request) || !work || !receipts || rank_count != 2U ||
        !valid_shared(observed, region_count) || !valid_shared(expected, region_count))
        return 1;
    const auto repetitions = request->repetitions;
    std::array<std::uint64_t, 2> completed{};
    bool complete = true;
    for (std::uint32_t rank = 0; rank < rank_count; ++rank) {
        const auto& receipt = receipts[rank];
        if (receipt.rank != rank || receipt.actual.repetitions > repetitions ||
            receipt.actual.work_units > repetitions)
            return 1;
        if (receipt.executed) completed[rank] = receipt.actual.repetitions;
        complete = complete && receipt.executed && receipt.execute_status == 0 &&
            completed[rank] == repetitions && receipt.actual.work_units == completed[rank] &&
            receipt.actual.output_count == (rank_receipt ? 2U : 0U);
    }
    *work = team_work(completed[0], completed[1]);
    if (!complete) return 1;
    if (compare_outputs(observed, expected, region_count) == 0) return 0;
    if constexpr (variants_change_state) {
        // Both algorithms produce the same logical work with different terminal control words.
        std::array<unsigned char, shared_bytes> alternate{};
        expected_terminal(alternate.data(), repetitions, true);
        return std::memcmp(observed[0].data, alternate.data(), alternate.size()) == 0 ? 0 : 1;
    }
    return 1;
}

auto team_observe(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* observed,
                  std::uint32_t region_count, const EpycBenchPackageRankReceiptV1* receipts,
                  std::uint32_t rank_count, EpycBenchPackageJsonV1* out) noexcept -> int {
    if (!valid_request(request) || !valid_shared(observed, region_count) || !receipts || rank_count != 2U)
        return 1;
    try {
        using Json = nlohmann::json;
        const auto word = [&](std::size_t offset) {
            std::uint64_t value{};
            std::memcpy(&value, observed[0].data + offset, sizeof(value));
            return value;
        };
        Json evidence{{"schema", "epycbench.package-shared-state-observation/v1"}, {"padding_intact", true},
            {"rank_completed", {receipts[0].actual.repetitions, receipts[1].actual.repetitions}}};
        if constexpr (task == CooperativeTask::atomic_counter) {
            evidence["counters"] = {word(0), word(kernel::shared_atomic_counter_stride)};
        } else if constexpr (task == CooperativeTask::false_sharing) {
            evidence["counters_by_offset"] = Json::array();
            for (const auto offset : kernel::false_sharing_counter_offsets)
                evidence["counters_by_offset"].push_back({{"offset", offset}, {"value", word(offset)}});
        } else if constexpr (task == CooperativeTask::lock_counter) {
            evidence["lock"] = word(kernel::lock_counter_offsets[0]);
            evidence["protected_total"] = word(kernel::lock_counter_offsets[1]);
            evidence["protected_rank_counts"] = {word(kernel::lock_counter_offsets[2]), word(kernel::lock_counter_offsets[3])};
        } else if constexpr (task == CooperativeTask::barrier) {
            evidence["arrivals"] = word(0);
            evidence["generation"] = word(8);
            evidence["cancelled"] = word(16);
            evidence["owner_generation"] = word(24);
            evidence["rank_generations"] = {word(64), word(128)};
            evidence["witnesses"] = {{word(72), word(80)}, {word(136), word(144)}};
        } else {
            evidence["generation"] = word(0);
            evidence["cancelled"] = word(8);
            evidence["published"] = word(64);
            evidence["consumed"] = word(128);
            evidence["slots"] = Json::array();
            for (std::size_t i = 0; i < kernel::queue_slots; ++i)
                evidence["slots"].push_back(word(192U + i * sizeof(std::uint64_t)));
        }
        return write_json(evidence.dump(), out);
    } catch (...) {
        return 1;
    }
}
} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_task_v1() noexcept
    -> const EpycBenchPackageTaskV1* {
    static constexpr EpycBenchPackageCollectiveTaskV1 collective{
        team_layout, team_initialize, team_reference, team_compare, team_observe};
    static constexpr EpycBenchPackageTaskV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageTaskV1), rank_layout, rank_initialize,
        rank_reference, rank_compare, &collective};
    return &descriptor;
}
