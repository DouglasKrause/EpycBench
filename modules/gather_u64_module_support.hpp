#pragma once

#include "epycbench/kernel/gather_u64_task.hpp"
#include "task_module_support.hpp"

// Each module pair accepts only the source layouts it declares.
#if defined(EPYCBENCH_GATHER_MATCHED_PAGES)
inline constexpr bool epycbench_gather_matched_page_module = true;
#else
inline constexpr bool epycbench_gather_matched_page_module = false;
#endif
#if defined(EPYCBENCH_GATHER_CACHE_SETS) || defined(EPYCBENCH_GATHER_MATCHED_PAGES)
inline constexpr bool epycbench_gather_cache_set_module = true;
#else
inline constexpr bool epycbench_gather_cache_set_module = false;
#endif

namespace epycbench::module_detail {
inline auto prepare_gather_u64(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (!request || !request->data || !request->repetitions ||
        request->bytes < kernel::gather_u64_header_bytes ||
        request->bytes > static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        reinterpret_cast<std::uintptr_t>(request->data) > UINTPTR_MAX - request->bytes)
        return 1;
    const auto* data = request->data;
    const auto pattern = load_sum_word(data, 3U);
    constexpr auto first_tag = epycbench_gather_matched_page_module ? 4U : (epycbench_gather_cache_set_module ? 2U : 0U);
    if (load_sum_word(data, 0U) != kernel::gather_u64_input_magic || pattern - first_tag > 1U)
        return 1;
    const auto values = load_sum_word(data, 1U), indices = load_sum_word(data, 2U);
    if constexpr (epycbench_gather_matched_page_module) {
        if (!kernel::valid_gather_matched_page_shape(values, indices)) return 1;
    } else if constexpr (epycbench_gather_cache_set_module) {
        if (!kernel::valid_gather_cache_set_table(values)) return 1;
    }
    if (!values || !indices || values > kernel::gather_u64_maximum_count ||
        indices > kernel::gather_u64_maximum_count ||
        request->bytes != kernel::gather_u64_header_bytes + 8U * (values + indices) ||
        request->repetitions > UINT64_MAX / indices)
        return 1;
    return 0;
}
inline auto valid_gather_u64_checksum(const EpycBenchKernelRequestV2& request,
                                      const std::uint64_t* checksum) noexcept -> bool {
    if (!checksum)
        return false;
    const auto input = reinterpret_cast<std::uintptr_t>(request.data),
               output = reinterpret_cast<std::uintptr_t>(checksum);
    return output <= UINTPTR_MAX - sizeof(*checksum) &&
           (output + sizeof(*checksum) <= input || input + request.bytes <= output);
}
[[gnu::always_inline]] inline auto gather_u64_index_valid(std::uint64_t index,
                                                          std::size_t values) noexcept -> bool {
    return index < values;
}
} // namespace epycbench::module_detail
