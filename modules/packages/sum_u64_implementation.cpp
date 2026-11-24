#include "task_u64_support.hpp"

#include "../output_task_module_support.hpp"

extern "C" auto epycbench_kernel_execute_v2(const EpycBenchKernelRequestV2*, std::uint64_t*)
    noexcept -> int;

namespace {

auto execute(const EpycBenchPackageCallV1* call, EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!call || !call->task || !work || call->input_count != 1 || call->output_count != 1 ||
        !call->inputs || !call->outputs || !call->outputs[0].data ||
        call->outputs[0].bytes != sizeof(std::uint64_t))
        return 1;
    const EpycBenchKernelRequestV2 request{
        call->inputs[0].data, call->inputs[0].bytes, call->task->repetitions};
    std::uint64_t checksum{};
    if (const auto status = epycbench_kernel_execute_v2(&request, &checksum); status != 0)
        return status;
    epycbench::module_detail::store_xor_word(call->outputs[0].data, 0, checksum);
    *work = {request.repetitions, (request.bytes / sizeof(std::uint64_t)) * request.repetitions, 1};
    return 0;
}

} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageImplementationV1), execute};
    return &descriptor;
}
