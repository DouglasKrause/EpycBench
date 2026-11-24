#pragma once

#include "epycbench/kernel/prefix_scan_u64_task.hpp"
#include "output_task_module_support.hpp"

namespace epycbench::module_detail {
inline auto prepare_prefix_scan_u64(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || !request->repetitions ||
        !request->input_bytes || request->input_bytes != request->output_bytes ||
        request->input_bytes % 8U != 0U || request->xor_mask != 0U ||
        request->input_bytes / 8U > kernel::prefix_scan_u64_maximum_count ||
        request->repetitions > UINT64_MAX / (request->input_bytes / 8U) ||
        byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes))
        return 1;
    return 0;
}
} // namespace epycbench::module_detail
