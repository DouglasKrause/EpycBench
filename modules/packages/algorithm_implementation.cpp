#include "algorithm_work.hpp"

#if defined(EPYCBENCH_BUNDLED_GATHER_TASK) || defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
extern "C" auto epycbench_kernel_execute_v2(const EpycBenchKernelRequestV2*, std::uint64_t*)
    noexcept -> int;
#elif defined(EPYCBENCH_BUNDLED_HISTOGRAM_TASK) && EPYCBENCH_HISTOGRAM_HASH
extern "C" auto epycbench_histogram_u64_execute_package(const EpycBenchKernelRequestV3*,
    std::uint64_t*, unsigned char*, std::uint64_t) noexcept -> int;
#else
extern "C" auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3*, std::uint64_t*)
    noexcept -> int;
#endif

namespace {

auto execute(const EpycBenchPackageCallV1* call, EpycBenchPackageWorkV1* work) noexcept -> int {
#if defined(EPYCBENCH_BUNDLED_GATHER_TASK) || defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
    constexpr auto checksum_index = 0U;
#else
    constexpr auto checksum_index = 1U;
#endif
    if (!call || !call->task || !work || call->input_count != 1 ||
        call->output_count != checksum_index + 1 || !call->inputs || !call->outputs ||
        !call->outputs[checksum_index].data || call->outputs[checksum_index].bytes != sizeof(std::uint64_t))
        return 1;
    const auto& input = call->inputs[0];
    const auto repetitions = call->task->repetitions;
    std::uint64_t checksum{};
#if defined(EPYCBENCH_BUNDLED_GATHER_TASK) || defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
    const EpycBenchKernelRequestV2 request{input.data, input.bytes, repetitions};
    const auto status = epycbench_kernel_execute_v2(&request, &checksum);
#else
    const EpycBenchKernelRequestV3 request{
        input.data, input.bytes, call->outputs[0].data, call->outputs[0].bytes, repetitions, 0};
#if defined(EPYCBENCH_BUNDLED_HISTOGRAM_TASK) && EPYCBENCH_HISTOGRAM_HASH
    if (call->scratch_count != 1U || !call->scratch) return 1;
    const auto status = epycbench_histogram_u64_execute_package(
        &request, &checksum, call->scratch[0].data, call->scratch[0].bytes);
#else
    const auto status = epycbench_kernel_execute_v3(&request, &checksum);
#endif
#endif
    if (status != 0) return status;
    const auto accounted = epycbench::bundled_packages::algorithm_work(input.data, input.bytes, call->outputs[0]);
    epycbench::module_detail::store_xor_word(call->outputs[checksum_index].data, 0, checksum);
    *work = {repetitions, accounted.units_per_repetition * repetitions, accounted.output_count};
    return 0;
}

} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageImplementationV1), execute};
    return &descriptor;
}
