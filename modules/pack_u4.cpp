#include "epycbench/kernel/pack_u4_task.hpp"
#include "output_task_module_support.hpp"

namespace {
using namespace epycbench;

auto valid_shape(const EpycBenchKernelRequestV3* request) noexcept -> bool {
    if (!request || !request->input || !request->output || request->xor_mask != 0U ||
        request->input_bytes == 0U || request->input_bytes > kernel::pack_u4_maximum_count ||
        request->output_bytes != (request->input_bytes + 1U) / 2U || request->repetitions == 0U ||
        request->repetitions > UINT64_MAX / request->input_bytes) return false;
    return !module_detail::byte_ranges_overlap(request->input, request->input_bytes, request->output, request->output_bytes);
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_pack_u4_body(const unsigned char* input, unsigned char* output,
                            std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t checksum{};
    for (std::uint64_t r{}; r < repetitions; ++r) {
        asm volatile("" ::: "memory");
        const auto pair = [&](std::size_t i) {
            const auto byte = static_cast<unsigned char>(input[i] | (input[i + 1U] << 4U));
            output[i / 2U] = byte;
            checksum += byte;
        };
        std::size_t i{};
#if EPYCBENCH_PACK_UNROLL4
        for (; i + 8U <= count; i += 8U) {
            pair(i); pair(i + 2U); pair(i + 4U); pair(i + 6U);
        }
#endif
        for (; i + 1U < count; i += 2U) pair(i);
        if (i < count) {
            output[i / 2U] = input[i];
            checksum += input[i];
        }
    }
    asm volatile("" ::: "memory");
    return checksum;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request, std::uint64_t* answer) noexcept -> int {
    if (!valid_shape(request) || !epycbench::module_detail::valid_xor_checksum(*request, answer)) return 1;
    *answer = epycbench_pack_u4_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes), request->repetitions);
    return 0;
}
