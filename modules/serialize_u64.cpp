#include "epycbench/kernel/serialize_u64_task.hpp"
#include "output_task_module_support.hpp"

namespace {
using namespace epycbench;
auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (!request || !request->input || !request->output || request->xor_mask != 0U ||
        request->input_bytes == 0U || request->input_bytes % 8U != 0U ||
        request->input_bytes > 8U * kernel::serialize_u64_maximum_count ||
        request->output_bytes != request->input_bytes || request->repetitions == 0U ||
        request->repetitions > UINT64_MAX / request->input_bytes) return 1;
    return module_detail::byte_ranges_overlap(request->input, request->input_bytes,
                                               request->output, request->output_bytes) ? 1 : 0;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_serialize_u64_body(const unsigned char* input, unsigned char* output,
                                  std::size_t records, std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t checksum{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        for (std::size_t index{}; index < records; ++index) {
            const auto value = epycbench::module_detail::load_sum_word(input, index);
            auto* wire = output + 8U * index;
#if EPYCBENCH_SERIALIZE_BSWAP
            auto network = value;
            if constexpr (std::endian::native == std::endian::little) network = std::byteswap(value);
            std::memcpy(wire, &network, sizeof(network));
#else
            for (unsigned byte{}; byte < 8U; ++byte)
                wire[byte] = static_cast<unsigned char>(value >> (56U - 8U * byte));
#endif
            checksum += value >> 56U;
        }
    }
    asm volatile("" ::: "memory");
    return checksum;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request, std::uint64_t* answer) noexcept -> int {
    if (prepare(request) != 0 || !epycbench::module_detail::valid_xor_checksum(*request, answer)) return 1;
    *answer = epycbench_serialize_u64_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U), request->repetitions);
    return 0;
}
