#include "task_u64_support.hpp"

#include "kernel/histogram_u64_leaf.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace {
namespace kernel = epycbench::kernel;
namespace leaf = kernel::detail::histogram_leaf;
using epycbench::bundled_packages::unsigned_parameter;
constexpr std::size_t receipt_bytes = 4U * sizeof(std::uint64_t);

auto configuration(const EpycBenchPackageTaskRequestV1& request)
    -> std::expected<kernel::HistogramU64Config, epycbench::core::Error> {
    return kernel::HistogramU64Config::create(unsigned_parameter(request, "record_count").value_or(0),
        unsigned_parameter(request, "input_seed").value_or(0) + request.seed);
}

auto writable(const EpycBenchPackageWriteRegionV1& region) noexcept -> leaf::Output {
    return {reinterpret_cast<std::byte*>(region.data), region.bytes};
}

auto readable(const EpycBenchPackageReadRegionV1& region) noexcept -> leaf::Input {
    return {reinterpret_cast<const std::byte*>(region.data), region.bytes};
}

auto layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* result) noexcept -> int {
    const auto config = configuration(*request);
    if (!config || request->rank_count != 2 || request->rank >= 2 || request->repetitions != 1) return 1;
    if (request->locality_count && request->locality[0].core == request->locality[1].core) return 1;
    const auto ranges = kernel::histogram_u64_rank_ranges(*config);
    const auto range = ranges[request->rank];
    *result = {};
    result->partition_begin = range.first_record;
    result->partition_count = range.record_count;
    return 0;
}

auto initialize(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageWriteRegionV1*,
                std::uint32_t count) noexcept -> int { return count == 0 ? 0 : 1; }

auto reference(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1*,
               std::uint32_t input_count, const EpycBenchPackageWriteRegionV1*, std::uint32_t output_count,
               EpycBenchPackageWorkV1* work) noexcept -> int {
    *work = {request->repetitions, request->partition_count, 0};
    return input_count == 0 && output_count == 0 ? 0 : 1;
}

auto compare(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1*,
             const EpycBenchPackageReadRegionV1*, std::uint32_t count) noexcept -> int { return count == 0 ? 0 : 1; }

auto shared_layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* result) noexcept -> int {
    const auto config = configuration(*request);
    if (!config || layout(request, result) != 0) return 1;
    *result = {4, {{"input", EPYCBENCH_PACKAGE_SHARED_INPUT, config->logical_bytes().value(), 64},
                   {"scratch", EPYCBENCH_PACKAGE_SHARED, kernel::histogram_u64_scratch_bytes, 64},
                   {"ordered", EPYCBENCH_PACKAGE_SHARED, config->output_bytes().value(), 64},
                   {"receipt", EPYCBENCH_PACKAGE_SHARED, receipt_bytes, 64}}, 0, 0};
    return 0;
}

auto shared_initialize(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageWriteRegionV1* regions,
                       std::uint32_t count) noexcept -> int {
    const auto config = configuration(*request);
    if (!config || count != 4) return 1;
    const auto input = writable(regions[0]);
    const auto output = writable(regions[2]);
    if (!kernel::initialize_histogram_u64_input(*config, input)) return 1;
    std::construct_at(reinterpret_cast<kernel::HistogramU64Scratch*>(regions[1].data));
    const auto expected = kernel::prepare_histogram_u64_reference(*config, input, output);
    if (!expected || !kernel::reset_histogram_u64_output(*expected, output)) return 1;
    leaf::write(writable(regions[3]), 0, ~expected->single_task_answer());
    return 0;
}

auto shared_reference(const EpycBenchPackageTaskRequestV1* request,
                      const EpycBenchPackageCollectiveInputV1* ranks, std::uint32_t rank_count,
                      const EpycBenchPackageWriteRegionV1* regions, std::uint32_t count,
                      EpycBenchPackageWorkV1* work) noexcept -> int {
    const auto config = configuration(*request);
    if (!config || rank_count != 2 || count != 4) return 1;
    const auto input = writable(regions[0]);
    if (!kernel::initialize_histogram_u64_input(*config, input)) return 1;
    const auto output = writable(regions[2]);
    const auto expected = kernel::prepare_histogram_u64_reference(*config, input, output);
    if (!expected) return 1;
    leaf::write(writable(regions[3]), 0, expected->single_task_answer());
    auto* partials = std::construct_at(reinterpret_cast<kernel::HistogramU64Scratch*>(regions[1].data));
    const auto ranges = kernel::histogram_u64_rank_ranges(*config);
    for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
        const auto [first, length] = ranges[rank];
        if (ranks[rank].input_count != 0 || ranks[rank].task->partition_begin != first ||
            ranks[rank].task->partition_count != length) return 1;
        for (std::uint64_t i = 0; i < length; ++i) {
            const auto word = leaf::read(input, kernel::histogram_u64_header_words + first + i);
            auto& bin = partials->ranks[rank].bins[word >> 56U];
            ++bin.count;
            bin.sum += word & leaf::weight_mask;
        }
    }
    *work = {1, config->record_count(), kernel::histogram_u64_bins};
    return 0;
}

auto shared_compare(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* observed,
                    const EpycBenchPackageReadRegionV1* expected, std::uint32_t count,
                    const EpycBenchPackageRankReceiptV1* ranks, std::uint32_t rank_count,
                    EpycBenchPackageWorkV1* work) noexcept -> int {
    const auto config = configuration(*request);
    if (!config || rank_count != 2 || count != 4) return 1;
    const auto ranges = kernel::histogram_u64_rank_ranges(*config);
    std::uint64_t consumed = 0;
    bool complete = true;
    for (std::size_t rank = 0; rank < ranges.size(); ++rank) {
        const auto& receipt = ranks[rank];
        complete = complete && receipt.rank == rank && receipt.executed == 1 && receipt.execute_status == 0 &&
            receipt.actual.repetitions == 1 && receipt.actual.work_units == ranges[rank].record_count &&
            receipt.actual.output_count == 0;
        if (receipt.actual.work_units > config->record_count() - consumed) return 1;
        consumed += receipt.actual.work_units;
    }
    const auto same = [](const auto& observed_region, const auto& expected_region) {
        return std::ranges::equal(readable(observed_region), readable(expected_region));
    };
    const auto scratch = readable(observed[1]);
    const auto ordered = readable(expected[2]);
    const bool private_partials = same(observed[1], expected[1]);
    const bool shared_atomic = std::ranges::equal(scratch.first(ordered.size()), ordered) &&
        std::ranges::all_of(scratch.subspan(ordered.size()), [](auto byte) { return byte == std::byte{0}; });
    const auto receipt = readable(observed[3]);
    const auto checksum_matches = leaf::read(receipt, 0) == leaf::read(readable(expected[3]), 0);
    const auto atomic_updates = leaf::read(receipt, 1);
    const auto plain_updates = leaf::read(receipt, 2);
    const auto combined = leaf::read(receipt, 3);
    const auto updates = 2U * config->record_count();
    const bool valid_shared = shared_atomic && atomic_updates == updates && plain_updates == 0 && combined == 0;
    const bool valid_private = private_partials && atomic_updates == 0 && plain_updates == updates &&
        combined == kernel::histogram_u64_bins;
    const bool output = same(observed[2], expected[2]) && checksum_matches;
    *work = {complete && output ? 1U : 0U, consumed, output ? kernel::histogram_u64_bins : 0U};
    return complete && output && (valid_private || valid_shared) ? 0 : 1;
}

auto observe(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1* regions, std::uint32_t count,
             const EpycBenchPackageRankReceiptV1*, std::uint32_t, EpycBenchPackageJsonV1* out) noexcept -> int {
    if (count != 4 || regions[3].bytes != receipt_bytes) return 1;
    try {
        const auto receipt = readable(regions[3]);
        const nlohmann::json observation{{"schema", "epycbench.histogram-two-rank-observation/v1"},
            {"checksum", leaf::read(receipt, 0)}, {"atomic_updates", leaf::read(receipt, 1)},
            {"plain_updates", leaf::read(receipt, 2)}, {"partial_bins_combined", leaf::read(receipt, 3)}};
        return epycbench::bundled_packages::write_json(observation.dump(), out);
    } catch (...) {
        return 1;
    }
}

constexpr EpycBenchPackageCollectiveTaskV1 collective{shared_layout, shared_initialize, shared_reference, shared_compare,
    observe};
constexpr EpycBenchPackageTaskV1 descriptor{EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageTaskV1),
    layout, initialize, reference, compare, &collective};
} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_task_v1() noexcept -> const EpycBenchPackageTaskV1* {
    return &descriptor;
}
