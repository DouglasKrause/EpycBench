#include "copy_bytes_module_support.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {

auto prepare_avx2(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (epycbench::module_detail::prepare_copy_bytes(request) != 0) return 1;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) return 0;
#endif
    return 2;
}

} // namespace

#if defined(__x86_64__)
extern "C" [[gnu::visibility("protected"), gnu::target("avx2"), gnu::noinline]]
auto epycbench_copy_bytes_avx2_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        std::size_t offset{};
        for (; count - offset >= sizeof(__m256i); offset += sizeof(__m256i)) {
            const auto value = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(input + offset));
            _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(output + offset), value);
        }
        epycbench::module_detail::copy_byte_tail(input, output, offset, count);
        folded += epycbench::module_detail::copied_last_byte(output, count);
    }
    return folded;
}
#endif

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    if (!request || !epycbench::module_detail::valid_xor_checksum(*request, checksum)) return 1;
    if (const auto status = prepare_avx2(request); status != 0) return status;
#if defined(__x86_64__)
    *checksum = epycbench_copy_bytes_avx2_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes), request->repetitions);
    return 0;
#else
    return 2;
#endif
}
