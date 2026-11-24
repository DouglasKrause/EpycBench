#include "task_u64_support.hpp"

#include "../output_task_module_support.hpp"
#include "kernel/mix64.hpp"

#include <cstring>

namespace {
using namespace epycbench::bundled_packages;

auto partition(const EpycBenchPackageTaskRequestV1* request) noexcept -> std::optional<Partition> {
    if (!request) return std::nullopt;
    const auto bytes = unsigned_parameter(*request, "byte_count");
    if (!bytes || *bytes > std::numeric_limits<std::size_t>::max() / 2U) return std::nullopt;
    return logical_partition(*request, *bytes);
}

auto layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* result)
    noexcept -> int {
    const auto slice = partition(request);
    if (!slice || !result) return 1;
    result->partition_begin = slice->begin;
    result->partition_count = slice->count;
    result->regions[0] = {"input", EPYCBENCH_PACKAGE_INPUT, slice->count, 64};
    result->regions[1] = {"output", EPYCBENCH_PACKAGE_OUTPUT, slice->count, 64};
    result->regions[2] = {"checksum", EPYCBENCH_PACKAGE_OUTPUT, sizeof(std::uint64_t), 64};
    result->region_count = 3;
    return 0;
}

auto initialize(const EpycBenchPackageTaskRequestV1* request,
                const EpycBenchPackageWriteRegionV1* inputs, std::uint32_t input_count)
    noexcept -> int {
    const auto slice = partition(request);
    if (!slice || input_count != 1 || !inputs || !inputs[0].data || inputs[0].bytes != slice->count)
        return 1;
    const auto seed = unsigned_parameter(*request, "input_seed");
    if (!seed) return 1;
    for (std::size_t index{}; index < slice->count; ++index)
        inputs[0].data[index] = static_cast<unsigned char>(
            epycbench::kernel::detail::mix64_v1(*seed + request->seed + slice->begin + index) & 0xffU);
    return 0;
}

auto reference(const EpycBenchPackageTaskRequestV1* request, const EpycBenchPackageReadRegionV1* inputs,
               std::uint32_t input_count, const EpycBenchPackageWriteRegionV1* output,
               std::uint32_t output_count, EpycBenchPackageWorkV1* work) noexcept -> int {
    const auto slice = partition(request);
    if (!slice || !work || input_count != 1 || !inputs || !inputs[0].data || inputs[0].bytes != slice->count ||
        output_count != 2 || !output || !output[0].data || output[0].bytes != slice->count ||
        !output[1].data || output[1].bytes != sizeof(std::uint64_t))
        return 1;
    std::memcpy(output[0].data, inputs[0].data, static_cast<std::size_t>(slice->count));
    epycbench::module_detail::store_xor_word(output[1].data, 0,
        static_cast<std::uint64_t>(inputs[0].data[slice->count - 1]) * request->repetitions);
    *work = {request->repetitions, slice->count * request->repetitions, slice->count};
    return 0;
}

auto compare(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1* observed,
             const EpycBenchPackageReadRegionV1* expected, std::uint32_t output_count) noexcept -> int {
    return output_count == 2U ? compare_outputs(observed, expected, output_count) : 1;
}
} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_task_v1() noexcept
    -> const EpycBenchPackageTaskV1* {
    static constexpr EpycBenchPackageTaskV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageTaskV1), layout, initialize, reference, compare, nullptr};
    return &descriptor;
}
