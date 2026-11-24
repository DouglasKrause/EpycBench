#pragma once

#include "../output_task_module_support.hpp"
#include "task_u64_support.hpp"

#if defined(EPYCBENCH_BUNDLED_ALGORITHM)
#include EPYCBENCH_ALGORITHM_HEADER
#endif

namespace epycbench::bundled_packages {

struct LegacyWork {
    std::uint64_t units_per_repetition;
    std::uint64_t output_count;
};

inline auto algorithm_work([[maybe_unused]] const unsigned char* input, [[maybe_unused]] std::uint64_t bytes,
    [[maybe_unused]] const EpycBenchPackageWriteRegionV1& output) noexcept -> LegacyWork {
    using module_detail::load_sum_word;
#if defined(EPYCBENCH_BUNDLED_AFFINE_TASK)
    static_cast<void>(input);
    const auto count = bytes / sizeof(std::uint64_t);
    return {count, count};
#elif defined(EPYCBENCH_BUNDLED_GATHER_TASK)
    static_cast<void>(bytes);
    return {load_sum_word(input, 2U), 1U};
#elif defined(EPYCBENCH_BUNDLED_GEMM_TASK)
    static_cast<void>(bytes);
    const auto output_count = load_sum_word(input, 1U) * load_sum_word(input, 3U);
    return {output_count * load_sum_word(input, 2U), output_count};
#elif defined(EPYCBENCH_BUNDLED_HISTOGRAM_TASK)
    static_cast<void>(bytes);
    return {load_sum_word(input, 1U), 256U};
#elif defined(EPYCBENCH_BUNDLED_SCATTER_TASK)
    static_cast<void>(bytes);
    const auto count = load_sum_word(input, 1U);
    return {count, count};
#elif defined(EPYCBENCH_ALGORITHM_BFS_U64)
    const auto visited = load_sum_word(output.data, kernel::bfs_u64_header_words);
    return {visited, visited};
#elif defined(EPYCBENCH_ALGORITHM_CSR_U64) || defined(EPYCBENCH_ALGORITHM_STABLE_KEY_SORT_U64) || \
    defined(EPYCBENCH_ALGORITHM_SEARCH_TREE_U64)
    const auto count = load_sum_word(input, 1U);
    return {count, count};
#elif defined(EPYCBENCH_ALGORITHM_TRANSPOSE_U64) || defined(EPYCBENCH_ALGORITHM_STENCIL_U64)
    const auto count = load_sum_word(input, 1U) * load_sum_word(input, 2U);
    return {count, count};
#elif defined(EPYCBENCH_ALGORITHM_CONVOLUTION_U64)
    const auto count = output.bytes / sizeof(std::uint64_t);
    return {count, count};
#elif defined(EPYCBENCH_ALGORITHM_COMPACT_U64)
    return {bytes / sizeof(std::uint64_t) - 1U, load_sum_word(output.data, 0U)};
#elif defined(EPYCBENCH_ALGORITHM_PACK_U4)
    return {bytes, output.bytes};
#elif defined(EPYCBENCH_ALGORITHM_SERIALIZE_U64) || defined(EPYCBENCH_ALGORITHM_MERGE_U64) || \
    defined(EPYCBENCH_ALGORITHM_PREFIX_SCAN_U64)
    const auto count = bytes / sizeof(std::uint64_t);
    return {count, count};
#elif defined(EPYCBENCH_ALGORITHM_VARINT14)
    // One one-byte and one two-byte record per three bytes of validated input.
    return {bytes / 3U * 2U, 1U};
#elif defined(EPYCBENCH_ALGORITHM_CHASE4_U64)
    return {kernel::chase4_u64_lane_count * (bytes / (kernel::chase4_u64_lane_count *
        kernel::chase4_u64_node_bytes) + 1U), 1U};
#elif defined(EPYCBENCH_ALGORITHM_DOT_F64)
    return {bytes / (2U * sizeof(double)), 1U};
#elif defined(EPYCBENCH_BUNDLED_CHECKSUM_ONLY)
    return {bytes / sizeof(std::uint64_t), 1U};
#endif
}

} // namespace epycbench::bundled_packages
