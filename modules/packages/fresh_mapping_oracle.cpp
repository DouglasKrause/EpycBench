#include "epycbench/kernel/package.h"

namespace {
auto verify(const EpycBenchPackageMappingExtentV1* extent, const unsigned char* data,
            uint64_t* verified) noexcept -> int {
    if (!extent || !verified || extent->bytes == 0U) return 1;
    *verified = 0U;
#ifdef EPYCBENCH_ALLOCATION_MAPPING
    return data || extent->pages != 0U || extent->page_bytes != 0U;
#else
    if (!data || extent->page_bytes == 0U || extent->bytes % extent->page_bytes != 0U ||
        extent->bytes / extent->page_bytes != extent->pages) return 1;
    for (uint64_t page{}; page < extent->pages; ++page)
        if (data[page * extent->page_bytes] == 0xa5U) ++*verified;
    return *verified != extent->pages;
#endif
}
} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_mapping_task_v1() noexcept
    -> const EpycBenchPackageMappingTaskV1* {
    static constexpr EpycBenchPackageMappingTaskV1 oracle{
        EPYCBENCH_PACKAGE_ABI_VERSION, sizeof(EpycBenchPackageMappingTaskV1), verify};
    return &oracle;
}
