#include "parameters.hpp"

#include <algorithm>
#include <limits>

namespace {
using byte_transform::parameter;

auto layout(const EpycBenchPackageTaskRequestV1* request, EpycBenchPackageLayoutV1* output)
    noexcept -> int {
    const auto elements = parameter(*request, "elements").value.unsigned_value;
    auto begin = request->partition_begin;
    auto count = request->partition_count;
    if (begin == 0 && count == 0) {
        const auto base = elements / request->rank_count;
        const auto remainder = elements % request->rank_count;
        const auto rank = static_cast<std::uint64_t>(request->rank);
        begin = rank * base + std::min(rank, remainder);
        count = base + (rank < remainder ? 1U : 0U);
    }
    if (begin >= elements || count == 0 || count > elements - begin ||
        request->repetitions > std::numeric_limits<std::uint64_t>::max() / count)
        return 1;

    *output = {3, {{"source", EPYCBENCH_PACKAGE_INPUT, count, 64},
                   {"transformed", EPYCBENCH_PACKAGE_OUTPUT, count, 64},
                   {"block", EPYCBENCH_PACKAGE_SCRATCH, 64, 64}}, begin, count};
    return 0;
}

auto initialize(const EpycBenchPackageTaskRequestV1* request,
                const EpycBenchPackageWriteRegionV1* regions, std::uint32_t count) noexcept -> int {
    if (count != 1) return 1;
    const auto elements = parameter(*request, "elements").value.unsigned_value;
    const auto pattern = byte_transform::view(parameter(*request, "pattern").value.enum_value);
    for (std::uint64_t slot = 0; slot < regions[0].bytes; ++slot) {
        const auto index = request->partition_begin + slot;
        const auto coordinate = pattern == "reverse" ? elements - 1U - index : index;
        const auto mixed = (coordinate + request->seed) * 0x9e3779b97f4a7c15ULL;
        regions[0].data[slot] = static_cast<unsigned char>((mixed >> 37U) ^ mixed);
    }
    return 0;
}

auto reference(const EpycBenchPackageTaskRequestV1* request,
               const EpycBenchPackageReadRegionV1* input, std::uint32_t input_count,
               const EpycBenchPackageWriteRegionV1* output, std::uint32_t output_count,
               EpycBenchPackageWorkV1* work) noexcept -> int {
    if (input_count != 1 || output_count != 1 || input[0].bytes != output[0].bytes) return 1;
    const auto scale = parameter(*request, "scale").value.float_value;
    const auto bias = parameter(*request, "bias").value.signed_value;
    const auto invert = parameter(*request, "invert").value.bool_value != 0;
    for (std::uint64_t i = 0; i < output[0].bytes; ++i) {
        const auto scaled = static_cast<std::int64_t>(static_cast<double>(input[0].data[i]) * scale);
        const auto modulo = ((scaled + bias) % 256 + 256) % 256;
        output[0].data[i] = static_cast<unsigned char>(invert ? 255 - modulo : modulo);
    }
    *work = {request->repetitions, output[0].bytes * request->repetitions, output[0].bytes};
    return 0;
}

auto compare(const EpycBenchPackageTaskRequestV1*, const EpycBenchPackageReadRegionV1* actual,
             const EpycBenchPackageReadRegionV1* expected, std::uint32_t count) noexcept -> int {
    if (count != 1 || actual[0].bytes != expected[0].bytes) return 1;
    return std::equal(actual[0].data, actual[0].data + actual[0].bytes, expected[0].data) ? 0 : 1;
}

constexpr EpycBenchPackageTaskV1 task{EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageTaskV1),
    layout, initialize, reference, compare, nullptr};
} // namespace

extern "C" auto epycbench_package_task_v1() noexcept -> const EpycBenchPackageTaskV1* {
    return &task;
}
