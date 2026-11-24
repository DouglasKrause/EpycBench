#include "epycbench/kernel/package.h"

#ifdef EPYCBENCH_ALLOCATION_MAPPING
extern "C" auto epycbench_package_request_anonymous_mapping(const EpycBenchPackageMappingExtentV1* extent,
    EpycBenchPackageMappingServiceV1 mapping) noexcept -> int {
    return mapping.allocate(mapping.context, extent->bytes);
}
#else
extern "C" auto epycbench_package_touch_fresh_pages(const EpycBenchPackageMappingExtentV1* extent,
    volatile unsigned char* bytes) noexcept -> int {
    const auto end = extent->bytes;
    const auto stride = extent->page_bytes;
    for (uint64_t offset{}; offset < end; offset += stride)
        bytes[offset] = 0xa5U;
    return 0;
}
#endif

extern "C" [[gnu::visibility("default")]] auto epycbench_package_mapping_implementation_v1() noexcept
    -> const EpycBenchPackageMappingImplementationV1* {
    static constexpr EpycBenchPackageMappingImplementationV1 implementation{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageMappingImplementationV1),
#ifdef EPYCBENCH_ALLOCATION_MAPPING
        nullptr, epycbench_package_request_anonymous_mapping
#else
        epycbench_package_touch_fresh_pages, nullptr
#endif
    };
    return &implementation;
}
