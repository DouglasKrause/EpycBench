#include "task_u64_support.hpp"

#include "../output_task_module_support.hpp"
#include "kernel/mix64.hpp"

#include <limits>

namespace {

#ifdef EPYCBENCH_BUNDLED_XOR_TASK
constexpr bool xor_task = true;
#else
constexpr bool xor_task = false;
#endif

auto partition(const EpycBenchPackageTaskRequestV1* request) noexcept
    -> std::optional<epycbench::bundled_packages::Partition> {
    if (!request) return std::nullopt;
    const auto elements = epycbench::bundled_packages::unsigned_parameter(*request, "element_count");
    constexpr auto maximum_words = std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t);
    if (!elements || *elements > maximum_words) return std::nullopt;
    return epycbench::bundled_packages::logical_partition(*request, *elements);
}

auto layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* result)
    noexcept -> int {
    const auto slice = partition(request);
    if (!slice || !result) return 1;
    const auto bytes = slice->count * sizeof(std::uint64_t);
    result->partition_begin = slice->begin;
    result->partition_count = slice->count;
    result->regions[0] = {"input", EPYCBENCH_PACKAGE_INPUT, bytes, 64};
    if constexpr (xor_task) {
        result->regions[1] = {"output", EPYCBENCH_PACKAGE_OUTPUT, bytes, 64};
        result->regions[2] = {"checksum", EPYCBENCH_PACKAGE_OUTPUT, sizeof(std::uint64_t), 64};
        result->region_count = 3;
    } else {
        result->regions[1] = {"checksum", EPYCBENCH_PACKAGE_OUTPUT, sizeof(std::uint64_t), 64};
        result->region_count = 2;
    }
    return 0;
}

auto initialize(const EpycBenchPackageTaskRequestV1* request,
                const EpycBenchPackageWriteRegionV1* inputs, std::uint32_t input_count)
    noexcept -> int {
    const auto slice = partition(request);
    if (!slice || input_count != 1 || !inputs || !inputs[0].data ||
        inputs[0].bytes != slice->count * sizeof(std::uint64_t))
        return 1;
    const auto seed = epycbench::bundled_packages::unsigned_parameter(*request, "input_seed");
    if (!seed) return 1;
    for (std::size_t index{}; index < slice->count; ++index) {
        const auto value = epycbench::kernel::detail::mix64_v1(*seed + request->seed + slice->begin + index);
        epycbench::module_detail::store_xor_word(inputs[0].data, index, value);
    }
    return 0;
}

auto reference(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* inputs,
               std::uint32_t input_count, const EpycBenchPackageWriteRegionV1* output,
               std::uint32_t output_count, EpycBenchPackageWorkV1* work) noexcept -> int {
    constexpr std::size_t checksum_index = xor_task ? 1U : 0U;
    const auto slice = partition(request);
    if (!slice || !work || input_count != 1 || !inputs || !inputs[0].data ||
        inputs[0].bytes != slice->count * sizeof(std::uint64_t) ||
        output_count != checksum_index + 1 || !output || !output[checksum_index].data ||
        output[checksum_index].bytes != sizeof(std::uint64_t))
        return 1;
    std::uint64_t mask{};
    if constexpr (xor_task) {
        const auto parameter = epycbench::bundled_packages::unsigned_parameter(*request, "xor_mask");
        if (!parameter || !output[0].data || output[0].bytes != inputs[0].bytes) return 1;
        mask = *parameter;
    }
    std::uint64_t sum{};
    for (std::size_t index{}; index < slice->count; ++index) {
        const auto value = epycbench::module_detail::load_sum_word(inputs[0].data, index) ^ mask;
        if constexpr (xor_task) epycbench::module_detail::store_xor_word(output[0].data, index, value);
        sum += value;
    }
    epycbench::module_detail::store_xor_word(output[checksum_index].data, 0, sum * request->repetitions);
    *work = {request->repetitions, slice->count * request->repetitions, xor_task ? slice->count : 1U};
    return 0;
}

auto compare(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1* observed,
             const EpycBenchPackageReadRegionV1* expected, std::uint32_t output_count) noexcept -> int {
    return output_count == (xor_task ? 2U : 1U)
        ? epycbench::bundled_packages::compare_outputs(observed, expected, output_count) : 1;
}

} // namespace

#ifdef EPYCBENCH_BUNDLED_COLLECTIVE_SUM
auto epycbench_collective_sum_task_api() noexcept -> const EpycBenchPackageCollectiveTaskV1*;
#endif

extern "C" [[gnu::visibility("default")]] auto epycbench_package_task_v1() noexcept
    -> const EpycBenchPackageTaskV1* {
#ifdef EPYCBENCH_BUNDLED_COLLECTIVE_SUM
    static const auto collective = epycbench_collective_sum_task_api();
#else
    constexpr auto collective = nullptr;
#endif
    static const EpycBenchPackageTaskV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageTaskV1), layout, initialize, reference, compare, collective};
    return &descriptor;
}
