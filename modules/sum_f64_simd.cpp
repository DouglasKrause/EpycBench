#include "dyadic_f64_module_support.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
auto prepare_avx(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (epycbench::module_detail::prepare_dyadic_f64(request) != 0) return 1;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx")) return 0;
#endif
    return 2;
}
} // namespace

#if defined(__x86_64__)
extern "C" [[gnu::visibility("protected"), gnu::target("avx,no-avx2,no-fma"), gnu::noinline]]
auto epycbench_sum_f64_avx_body(const unsigned char* input, std::size_t elements,
    std::uint64_t repetitions) noexcept -> std::uint64_t {
    static_assert(std::endian::native == std::endian::little);
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        auto accumulator = _mm256_setzero_pd();
        std::size_t index{};
        for (; elements - index >= 4U; index += 4U) {
            __m256d values;
            std::memcpy(&values, input + index * 8U, sizeof(values));
            accumulator = _mm256_add_pd(accumulator, values);
        }
        const auto halves = _mm_add_pd(_mm256_castpd256_pd128(accumulator),
                                      _mm256_extractf128_pd(accumulator, 1));
        double sum = _mm_cvtsd_f64(halves) + _mm_cvtsd_f64(_mm_unpackhi_pd(halves, halves));
        for (; index < elements; ++index)
            sum += epycbench::module_detail::load_dyadic_f64(input, index);
        folded += epycbench::module_detail::canonical_dyadic_f64_bits(sum);
    }
    asm volatile("" ::: "memory");
    return folded;
}
#endif

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (!request || !epycbench::module_detail::valid_dyadic_f64_checksum(*request, checksum)) return 1;
    if (const auto status = prepare_avx(request); status != 0) return status;
#if defined(__x86_64__)
    *checksum = epycbench_sum_f64_avx_body(request->data,
        static_cast<std::size_t>(request->bytes / 8U), request->repetitions);
    return 0;
#else
    return 2;
#endif
}
