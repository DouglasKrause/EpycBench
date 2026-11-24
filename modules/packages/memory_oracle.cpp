#include "task_u64_support.hpp"
#include "epycbench/core/seed.hpp"
#include "kernel/canonical_latency_permutation.hpp"

#include <cstring>

namespace {
#ifdef EPYCBENCH_PACKAGE_LATENCY
constexpr bool latency=true;
#else
constexpr bool latency=false;
#endif
using Request=EpycBenchPackageTaskRequestV1;
using U64=std::uint64_t;

int layout(const Request* request,EpycBenchPackageLayoutV1* out) noexcept {
    if (!request || !out) return 1;
    const auto lines=epycbench::bundled_packages::unsigned_parameter(*request,"working_set_lines");
    if (!lines || *lines<(latency ? 2U:64U) || *lines>UINT64_MAX/64U ||
        (!latency && request->repetitions>UINT64_MAX/(64U * *lines)) || request->rank_count>UINT64_MAX/ *lines) return 1;
    out->partition_begin=static_cast<U64>(request->rank)* *lines;
    out->partition_count=*lines;
    out->regions[0]={"input",EPYCBENCH_PACKAGE_INPUT,64U * *lines,64};
    if constexpr (latency) {
        out->regions[1]={"start",EPYCBENCH_PACKAGE_INPUT,8,8};
        out->regions[2]={"terminal_index",EPYCBENCH_PACKAGE_OUTPUT,8,8};
        out->region_count=3;
        out->preparation_bytes=*lines*sizeof(std::uint64_t);
    } else {
        out->regions[1]={"checksum",EPYCBENCH_PACKAGE_OUTPUT,8,8};
        out->region_count=2;
    }
    return 0;
}

int initialize(const Request* request,const EpycBenchPackageWriteRegionV1* inputs,std::uint32_t count) noexcept {
    if (!request || !inputs || count!=(latency ? 2U:1U)) return 1;
    try {
        if constexpr (latency) {
            const auto n=inputs[0].bytes/64U;
            const auto chain=epycbench::bundled_packages::unsigned_parameter(*request,"chain_seed");
            const auto root=epycbench::bundled_packages::unsigned_parameter(*request,"start_seed");
            if (n<2 || !chain || !root || inputs[1].bytes!=8) return 1;
            const auto order=epycbench::kernel::detail::canonical_latency_permutation(n,*chain);
            if (!order) return 1;
            for (std::size_t i=0;i<n;++i) {
                const auto next=(*order)[(i+1U)%n];
                std::memcpy(inputs[0].data+64U*(*order)[i],&next,sizeof(next));
            }
            const auto seed=epycbench::core::derive_seed(*root,{epycbench::core::SeedDomain::observation_start,
                {{"experiment","0"},{"block",std::to_string(request->block)},{"component","memory.latency.dependent-load/v1"}}});
            if (!seed) return 1;
            for (U64 i=0;i<UINT64_MAX;++i) {
                const auto start=epycbench::kernel::detail::bounded_index(epycbench::kernel::detail::mix64_v1(seed->value()+i),n);
                if (start) { std::memcpy(inputs[1].data,&*start,sizeof(*start)); return 0; }
            }
            return 1;
        } else {
            for (U64 i=0;i<inputs[0].bytes/8U;++i) {
                const auto value=epycbench::kernel::detail::mix64_v1(i);
                std::memcpy(inputs[0].data+i*8U,&value,sizeof(value));
            }
            return 0;
        }
    } catch (const std::exception&) { return 1; }
}

int reference(const Request* request,const EpycBenchPackageReadRegionV1* inputs,std::uint32_t n,
              const EpycBenchPackageWriteRegionV1* outputs,std::uint32_t m,EpycBenchPackageWorkV1* work) noexcept {
    if (!request || !inputs || !outputs || !work || n!=(latency?2U:1U) || m!=1 || outputs[0].bytes!=8) return 1;
    U64 expected{},units{};
    if constexpr (latency) {
        const auto nodes=inputs[0].bytes/64U;
        U64 start{};
        std::memcpy(&start,inputs[1].data,8);
        if (start>=nodes || nodes<2) return 1;
        // Verify one complete cycle and select the expected terminal outside the timed call.
        auto index=start;
        const auto steps=request->repetitions%nodes;
        for (U64 step=0;step<nodes;++step) {
            if (step==steps) expected=index;
            std::memcpy(&index,inputs[0].data+index*64U,8);
            if (index>=nodes || (index==start && step+1U!=nodes)) return 1;
        }
        if (index!=start) return 1;
        units=request->repetitions;
    } else {
        for (U64 i=0;i<inputs[0].bytes;i+=8U) {
            U64 value;
            std::memcpy(&value,inputs[0].data+i,8);
            expected+=value;
        }
        expected*=request->repetitions;
        units=inputs[0].bytes*request->repetitions;
    }
    std::memcpy(outputs[0].data,&expected,8);
    *work={request->repetitions,units,1};
    return 0;
}

int compare(const Request*,const EpycBenchPackageReadRegionV1* actual,const EpycBenchPackageReadRegionV1* expected,
            std::uint32_t count) noexcept {
    return count==1 ? epycbench::bundled_packages::compare_outputs(actual,expected,count):1;
}
}
extern "C" [[gnu::visibility("default")]] auto epycbench_package_task_v1() noexcept -> const EpycBenchPackageTaskV1* {
    static constexpr EpycBenchPackageTaskV1 task{EPYCBENCH_PACKAGE_ABI_VERSION,sizeof(EpycBenchPackageTaskV1),
        layout,initialize,reference,compare,nullptr};
    return &task;
}
