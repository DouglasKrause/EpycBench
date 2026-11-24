#include "collective_sum_support.hpp"

#include "../output_task_module_support.hpp"
#include "epycbench/kernel/reduction_tree.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <vector>

namespace {
using namespace epycbench::bundled_packages;
using namespace epycbench::bundled_packages::collective_sum;
namespace core = epycbench::core;
namespace kernel = epycbench::kernel;

auto layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* result) noexcept -> int {
    if (!valid_team(request) || !result) return 1;
    result->region_count = 2;
    result->regions[0] = {"partials", EPYCBENCH_PACKAGE_SHARED, state_bytes(request->rank_count), line};
    result->regions[1] = {"reduction_tree", EPYCBENCH_PACKAGE_SHARED_INPUT, schedule_bytes(request->rank_count), line};
    result->partition_begin = 0;
    result->partition_count = 0;
    return 0;
}

auto initialize(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageWriteRegionV1* regions,
                std::uint32_t count) noexcept -> int {
    try {
        if (!valid_team(request) || !regions || count != 2 || !regions[0].data || !regions[1].data ||
            regions[0].bytes != state_bytes(request->rank_count) || regions[1].bytes != schedule_bytes(request->rank_count))
            return 1;
        std::vector<kernel::ReductionRankLocality> locality;
        locality.reserve(request->rank_count);
        for (std::uint32_t rank = 0; rank < request->rank_count; ++rank) {
            const auto& location = request->locality[rank];
            if (location.package == UINT32_MAX) return 1;
            locality.push_back({core::PackageId{location.package},
                location.linux_numa_node == UINT32_MAX ? std::nullopt : std::optional{core::NumaNodeId{location.linux_numa_node}},
                location.llc == UINT32_MAX ? std::nullopt : std::optional{core::CacheId{location.llc}}});
        }
        const auto plan = kernel::ReductionTreePlan::create(locality, maximum_ranks);
        if (!plan) return 1;

        auto* state = regions[0].data;
        std::memset(state, 0, static_cast<std::size_t>(regions[0].bytes));
        std::construct_at(reinterpret_cast<Control*>(state));
        auto* tree = regions[1].data;
        std::memset(tree, 0, static_cast<std::size_t>(regions[1].bytes));
        auto* summary = std::construct_at(reinterpret_cast<Schedule*>(tree));
        std::vector<std::uint32_t> leader_slots;
        for (std::uint32_t rank = 0; rank < request->rank_count; ++rank) leader_slots.push_back(rank);
        std::uint32_t next_slot = request->rank_count;
        std::uint32_t next_source = 0;
        std::uint32_t stage = 0;
        for (const auto& level : plan->levels()) {
            bool merged = false;
            for (const auto& group : level.groups) {
                if (group.children.size() < 2) continue;
                if (group.children.size() + next_source > maximum_stages * request->rank_count ||
                    next_slot >= 2U * request->rank_count - 1U || stage >= maximum_stages)
                    return 1;
                const auto leader = group.leader();
                const Merge merge{next_source, static_cast<std::uint32_t>(group.children.size()), next_slot};
                std::memcpy(merge_bytes(tree, stage * request->rank_count + leader), &merge, sizeof(merge));
                for (const auto child : group.children) {
                    std::memcpy(source_bytes(tree, request->rank_count, next_source++), &leader_slots[child],
                                sizeof(std::uint32_t));
                }
                leader_slots[leader] = next_slot++;
                merged = true;
            }
            if (merged) ++stage;
        }
        *summary = {request->rank_count, stage, leader_slots[0]};
        return 0;
    } catch (...) {
        return 1;
    }
}

auto reference(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageCollectiveInputV1* ranks,
               std::uint32_t rank_count, const EpycBenchPackageWriteRegionV1* output,
               std::uint32_t count, EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!valid_team(request) || !ranks || rank_count != request->rank_count || !work ||
        initialize(request, output, count) != 0)
        return 1;
    const auto total = *unsigned_parameter(*request, "element_count");
    std::uint64_t answer{};
    auto* expected = reinterpret_cast<Control*>(output[0].data);
    for (std::uint32_t rank = 0; rank < rank_count; ++rank) {
        const auto& local = ranks[rank];
        if (!local.task || local.task->rank != rank || !local.inputs || local.input_count != 1U)
            return 1;
        auto canonical = *local.task;
        canonical.partition_begin = 0;
        canonical.partition_count = 0;
        const auto slice = logical_partition(canonical, total);
        if (!slice || local.task->partition_begin != slice->begin || local.task->partition_count != slice->count ||
            !local.inputs[0].data || local.inputs[0].bytes != slice->count * sizeof(std::uint64_t))
            return 1;
        std::uint64_t partial{};
        for (std::size_t index = 0; index < slice->count; ++index)
            partial += epycbench::module_detail::load_sum_word(local.inputs[0].data, index);
        store_partial(output[0].data, rank, partial);
        answer += partial;
    }
    const auto repetitions = request->repetitions;
    expected->combined = repetitions;
    expected->checksum = answer * repetitions;
    *work = {repetitions, total * repetitions, 1};
    return 0;
}

auto compare(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* observed,
             const EpycBenchPackageReadRegionV1* expected, std::uint32_t region_count,
             const EpycBenchPackageRankReceiptV1* receipts, std::uint32_t rank_count,
             EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!valid_team(request) || !observed || !expected || region_count != 2 || !receipts || !work ||
        rank_count != request->rank_count || !observed[0].data || !expected[0].data ||
        observed[0].bytes != state_bytes(rank_count) || expected[0].bytes != observed[0].bytes ||
        !observed[1].data || !expected[1].data || observed[1].bytes != schedule_bytes(rank_count) ||
        expected[1].bytes != observed[1].bytes)
        return 1;
    const auto& actual = *reinterpret_cast<const Control*>(observed[0].data);
    const auto& target = *reinterpret_cast<const Control*>(expected[0].data);
    const auto repetitions = request->repetitions;
    const auto elements = *unsigned_parameter(*request, "element_count");
    std::uint64_t processed{};
    bool complete = true;
    for (std::uint32_t rank = 0; rank < rank_count; ++rank) {
        const auto& receipt = receipts[rank];
        auto canonical = *request;
        canonical.rank = rank;
        canonical.partition_begin = 0;
        canonical.partition_count = 0;
        const auto slice = logical_partition(canonical, elements);
        if (!slice || receipt.rank != rank || receipt.actual.repetitions > repetitions ||
            receipt.actual.work_units > slice->count * receipt.actual.repetitions)
            return 1;
        if (receipt.executed) processed += receipt.actual.work_units;
        complete = complete && receipt.executed && receipt.execute_status == 0 &&
            receipt.actual.repetitions == repetitions && receipt.actual.work_units == slice->count * repetitions &&
            receipt.actual.output_count == 1 &&
            load_partial(observed[0].data, rank) == load_partial(expected[0].data, rank);
    }
    *work = {actual.combined, processed, actual.combined == 0 ? 0U : 1U};
    const auto stages = reinterpret_cast<const Schedule*>(expected[1].data)->stages;
    const auto phase_count = actual.epoch / repetitions;
    return complete && actual.cancelled == 0 && actual.arrivals == 0 && actual.combined == target.combined &&
        actual.checksum == target.checksum && actual.epoch % repetitions == 0 &&
        (phase_count == 2U || phase_count == stages + 1U) &&
        std::memcmp(observed[1].data, expected[1].data, schedule_bytes(rank_count)) == 0 ? 0 : 1;
}

auto observe(const EpycBenchPackageTaskRequestV1* request,const EpycBenchPackageReadRegionV1* observed,
             std::uint32_t region_count,const EpycBenchPackageRankReceiptV1* receipts,std::uint32_t rank_count,
             EpycBenchPackageJsonV1* out) noexcept -> int {
    if (!valid_team(request) || !observed || !receipts || !out || region_count!=2U ||
        rank_count!=request->rank_count || !observed[0].data || observed[0].bytes!=state_bytes(rank_count)) return 1;
    try {
        const auto& state=*reinterpret_cast<const Control*>(observed[0].data);
        auto partials=nlohmann::json::array();
        for (std::uint32_t rank=0;rank<rank_count;++rank) {
            if (receipts[rank].rank!=rank || !receipts[rank].executed || receipts[rank].execute_status!=0) return 1;
            partials.push_back(load_partial(observed[0].data,rank));
        }
        const nlohmann::json facts{{"schema","epycbench.collective-sum-observation/v1"},
            {"completed_repetitions",state.combined},{"checksum",state.checksum},{"epoch",state.epoch},
            {"pending_arrivals",state.arrivals},{"cancelled",state.cancelled!=0},{"partials",std::move(partials)}};
        return write_json(facts.dump(),out);
    } catch (...) {
        return 1;
    }
}
} // namespace

auto epycbench_collective_sum_task_api() noexcept -> const EpycBenchPackageCollectiveTaskV1* {
    static constexpr EpycBenchPackageCollectiveTaskV1 value{layout, initialize, reference, compare, observe};
    return &value;
}
