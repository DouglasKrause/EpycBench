#include "task_u64_support.hpp"

#include "../output_task_module_support.hpp"

extern "C" auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3*, std::uint64_t*)
    noexcept -> int;

namespace {

auto execute(const EpycBenchPackageCallV1* call, EpycBenchPackageWorkV1* work) noexcept -> int {
    if (!call || !call->task || !work || call->input_count != 1 || call->output_count != 2 ||
        !call->inputs || !call->outputs || !call->outputs[1].data ||
        call->outputs[1].bytes != sizeof(std::uint64_t))
        return 1;
#ifdef EPYCBENCH_BUNDLED_COPY_TASK
    constexpr auto mask = std::optional<std::uint64_t>{0};
    constexpr auto element_bytes = 1U;
#else
    const auto mask = epycbench::bundled_packages::unsigned_parameter(*call->task, "xor_mask");
    constexpr auto element_bytes = sizeof(std::uint64_t);
#endif
    if (!mask) return 1;
    const EpycBenchKernelRequestV3 request{
        call->inputs[0].data, call->inputs[0].bytes, call->outputs[0].data, call->outputs[0].bytes,
        call->task->repetitions, *mask};
    std::uint64_t checksum{};
    if (const auto status = epycbench_kernel_execute_v3(&request, &checksum); status != 0)
        return status;
    epycbench::module_detail::store_xor_word(call->outputs[1].data, 0, checksum);
    const auto count = request.input_bytes / element_bytes;
    *work = {request.repetitions, count * request.repetitions, count};
    return 0;
}

} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 descriptor{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageImplementationV1), execute};
    return &descriptor;
}
