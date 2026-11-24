#include "epycbench/kernel/package.h"
#include "../leaf_requests.hpp"

#include <cstdint>
#include <cstring>

extern "C" auto epycbench_kernel_execute_v1(const EpycBenchKernelRequestV1*) noexcept -> std::uint64_t;
namespace {
int execute(const EpycBenchPackageCallV1* call,EpycBenchPackageWorkV1* work) noexcept {
    if (!call || !call->task || !work || !call->inputs || call->output_count!=1 || !call->outputs) return 1;
    const auto iterations=call->task->repetitions;
    std::uint64_t start{},units{};
#ifdef EPYCBENCH_PACKAGE_LATENCY
    if (call->input_count!=2 || call->inputs[1].bytes!=8) return 1;
    std::memcpy(&start,call->inputs[1].data,8);
    units=iterations;
#else
    if (call->input_count!=1) return 1;
    units=iterations*call->inputs[0].bytes;
#endif
    const EpycBenchKernelRequestV1 request{call->inputs[0].data,call->inputs[0].bytes,iterations,start};
    const auto answer=epycbench_kernel_execute_v1(&request);
    std::memcpy(call->outputs[0].data,&answer,8);
    *work={iterations,units,1};
    return 0;
}
}
extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 body{EPYCBENCH_PACKAGE_ABI_VERSION,
        sizeof(EpycBenchPackageImplementationV1),execute,nullptr};
    return &body;
}
