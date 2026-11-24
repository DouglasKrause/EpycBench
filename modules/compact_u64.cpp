#include "epycbench/kernel/compact_u64_task.hpp"
#include "output_task_module_support.hpp"

#include <array>
#include <bit>

namespace {
using namespace epycbench;
using namespace epycbench::module_detail;
auto prepare_compact_u64(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || !request->repetitions || request->xor_mask != 0U ||
        request->input_bytes < 16U || request->input_bytes != request->output_bytes || request->input_bytes % 8U != 0U)
        return 1;
    const auto elements = request->input_bytes / 8U - 1U;
    if (elements > kernel::compact_u64_maximum_count || request->repetitions > UINT64_MAX / elements ||
        byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes))
        return 1;
    return load_sum_word(request->input, 0U) == elements ? 0 : 1;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_compact_u64_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::size_t kept{}, i{};
        std::uint64_t sum{};
#if EPYCBENCH_COMPACT_MASK4
        for (; count - i >= 4U; i += 4U) {
            std::array<std::uint64_t, 4> values{};
            unsigned mask{};
            for (unsigned lane{}; lane < values.size(); ++lane) {
                values[lane] = load_sum_word(input, i + lane + 1U);
                mask |= static_cast<unsigned>((values[lane] & 1U) == 0U) << lane;
            }
            while (mask != 0U) {
                const auto lane = static_cast<unsigned>(std::countr_zero(mask));
                store_xor_word(output, ++kept, values[lane]);
                sum += values[lane];
                mask &= mask - 1U;
            }
        }
#endif
        for (; i < count; ++i) {
            const auto value = load_sum_word(input, i + 1U);
            if ((value & 1U) == 0U) {
                store_xor_word(output, ++kept, value);
                sum += value;
            }
        }
        store_xor_word(output, 0U, kept);
        folded += sum + kept;
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    using namespace epycbench::module_detail;
    if (prepare_compact_u64(request) != 0 || !valid_xor_checksum(*request, checksum)) return 1;
    *checksum = epycbench_compact_u64_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U - 1U), request->repetitions);
    return 0;
}
