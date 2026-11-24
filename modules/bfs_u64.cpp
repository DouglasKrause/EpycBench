#include "epycbench/kernel/bfs_u64_task.hpp"
#include "output_task_module_support.hpp"

#include <bit>

namespace {
using namespace epycbench;
constexpr auto header = kernel::bfs_u64_header_words;
auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->xor_mask ||
        !request->repetitions || request->input_bytes < 8U * header ||
        module_detail::load_sum_word(request->input, 0U) != kernel::bfs_u64_input_magic)
        return 1;
    const auto vertices = module_detail::load_sum_word(request->input, 1U);
    const auto edges = module_detail::load_sum_word(request->input, 2U);
    const auto source = module_detail::load_sum_word(request->input, 3U);
    if (vertices < 8U || vertices > kernel::bfs_u64_maximum_vertices ||
        !std::has_single_bit(vertices) || source >= vertices)
        return 1;
    const auto reached = 3U * vertices / 4U, scanned = kernel::bfs_u64_component_edges(reached);
    if (edges != scanned + kernel::bfs_u64_component_edges(vertices - reached) ||
        request->input_bytes != 8U * (header + vertices + 1U + edges) ||
        request->output_bytes != 8U * (header + 2U + 2U * vertices) ||
        request->repetitions > UINT64_MAX / scanned ||
        module_detail::byte_ranges_overlap(request->input, request->input_bytes, request->output,
                                           request->output_bytes))
        return 1;
    return 0;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_bfs_u64_body(const unsigned char* input, unsigned char* output,
                            std::uint64_t repetitions, std::uint64_t* checksum) noexcept -> int {
    using namespace epycbench::module_detail;
    const auto vertices = load_sum_word(input, 1U), edges = load_sum_word(input, 2U),
               source = load_sum_word(input, 3U);
    const auto adjacency = header + vertices + 1U, distances = header + 2U,
               queue = distances + vertices;
    const auto reached = 3U * vertices / 4U,
               expected_scans = epycbench::kernel::bfs_u64_component_edges(reached);
    std::uint64_t folded{};
    for (std::uint64_t rep{}; rep < repetitions; ++rep) {
        asm volatile("" ::: "memory");
        for (std::size_t i{}; i < header; ++i)
            store_xor_word(output, i, load_sum_word(input, i));
        for (std::uint64_t i{}; i < vertices; ++i) {
            store_xor_word(output, distances + i, UINT64_MAX);
            store_xor_word(output, queue + i, UINT64_MAX);
        }
        if (load_sum_word(input, header) != 0U || load_sum_word(input, header + vertices) != edges)
            return 1;
        store_xor_word(output, distances + source, 0U);
        store_xor_word(output, queue, source);
        std::uint64_t tail{1U}, scanned{};
        folded += source + 1U;
        for (std::uint64_t head{}; head < tail; ++head) {
            const auto v = load_sum_word(output, queue + head);
            const auto level = load_sum_word(output, distances + v);
            auto pos = load_sum_word(input, header + v);
            const auto end = load_sum_word(input, header + v + 1U);
            if (pos > end || end > edges)
                return 1;
            scanned += end - pos;
            const auto discover = [&](std::uint64_t at) {
                const auto next = load_sum_word(input, adjacency + at);
                if (next >= vertices)
                    return false;
                if (load_sum_word(output, distances + next) == UINT64_MAX) {
                    if (tail == vertices)
                        return false;
                    store_xor_word(output, distances + next, level + 1U);
                    store_xor_word(output, queue + tail++, next);
                    folded += (next + 1U) * (level + 2U);
                }
                return true;
            };
#if EPYCBENCH_BFS_UNROLL4
            for (; end - pos >= 4U; pos += 4U)
                if (!discover(pos) || !discover(pos + 1U) || !discover(pos + 2U) ||
                    !discover(pos + 3U))
                    return 1;
#endif
            for (; pos < end; ++pos)
                if (!discover(pos))
                    return 1;
        }
        if (tail != reached || scanned != expected_scans)
            return 1;
        store_xor_word(output, header, tail);
        store_xor_word(output, header + 1U, scanned);
    }
    asm volatile("" ::: "memory");
    *checksum = folded;
    return 0;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                                 std::uint64_t* answer) noexcept -> int {
    if (prepare(request) != 0 || !epycbench::module_detail::valid_xor_checksum(*request, answer))
        return 1;
    return epycbench_bfs_u64_body(request->input, request->output, request->repetitions, answer);
}
