#include "dot_f64_module_support.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
auto prepare_avx(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (epycbench::module_detail::prepare_dot_f64(request) != 0) return 1;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx")) return 0;
#endif
    return 2;
}
} // namespace

#if defined(__x86_64__)
extern "C" [[gnu::visibility("protected"), gnu::target("avx,no-avx2,no-fma"), gnu::noinline]]
auto epycbench_dot_f64_avx_body(const unsigned char* input, std::size_t pairs,
    std::uint64_t repetitions) noexcept -> std::uint64_t {
    static_assert(std::endian::native == std::endian::little);
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        auto accumulator = _mm256_setzero_pd();
        std::size_t index{};
        for (; pairs - index >= 4U; index += 4U) {
            __m256d first, second;
            std::memcpy(&first, input + index * 16U, sizeof(first));
            std::memcpy(&second, input + index * 16U + sizeof(first), sizeof(second));
            // Unpack the interleaved pairs into corresponding x/y lanes.
            // Their order is 0,2,1,3; all products/partial sums are exact.
            const auto x = _mm256_unpacklo_pd(first, second);
            const auto y = _mm256_unpackhi_pd(first, second);
            const auto products = _mm256_mul_pd(x, y);
            accumulator = _mm256_add_pd(accumulator, products);
        }
        const auto halves = _mm_add_pd(_mm256_castpd256_pd128(accumulator),
                                      _mm256_extractf128_pd(accumulator, 1));
        double sum = _mm_cvtsd_f64(halves) +
                     _mm_cvtsd_f64(_mm_unpackhi_pd(halves, halves));
        for (; index < pairs; ++index) {
            const auto x = std::bit_cast<double>(epycbench::module_detail::load_sum_word(input, index * 2U));
            const auto y = std::bit_cast<double>(epycbench::module_detail::load_sum_word(input, index * 2U + 1U));
            const double product = x * y;
            sum += product;
        }
        folded += epycbench::module_detail::canonical_dot_f64_bits(sum);
    }
    asm volatile("" ::: "memory");
    return folded;
}
#endif

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (!request || !epycbench::module_detail::valid_dot_f64_checksum(*request, checksum)) return 1;
    if (const auto status = prepare_avx(request); status != 0) return status;
#if defined(__x86_64__)
    *checksum = epycbench_dot_f64_avx_body(request->data,
        static_cast<std::size_t>(request->bytes / 16U), request->repetitions);
    return 0;
#else
    return 2;
#endif
}
