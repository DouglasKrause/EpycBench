#include "u64_extrema_module_support.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
auto prepare_avx2(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (epycbench::module_detail::prepare_sum_u64(request) != 0) return 1;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) return 0;
#endif
    return 2;
}
} // namespace

#if defined(__x86_64__)
extern "C" [[gnu::visibility("protected"), gnu::target("avx2"), gnu::noinline]]
auto epycbench_min_u64_avx2_body(const unsigned char* input, std::size_t count,
    std::uint64_t repetitions) noexcept -> std::uint64_t {
    static_assert(std::endian::native == std::endian::little);
    const auto sign = _mm256_set1_epi64x(std::numeric_limits<std::int64_t>::min());
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        auto minima = _mm256_set1_epi64x(-1);
        std::size_t index{};
        for (; count - index >= 4U; index += 4U) {
            __m256i words;
            std::memcpy(&words, input + index * 8U, sizeof(words));
            // XOR maps unsigned ordering onto AVX2's signed 64-bit comparison.
            const auto smaller = _mm256_cmpgt_epi64(_mm256_xor_si256(minima, sign),
                                                     _mm256_xor_si256(words, sign));
            minima = _mm256_blendv_epi8(minima, words, smaller);
        }
        std::uint64_t lanes[4];
        std::memcpy(lanes, &minima, sizeof(lanes));
        auto minimum = std::numeric_limits<std::uint64_t>::max();
        for (const auto word : lanes) minimum = word < minimum ? word : minimum;
        for (; index < count; ++index) {
            const auto word = epycbench::module_detail::load_sum_word(input, index);
            minimum = word < minimum ? word : minimum;
        }
        folded += minimum;
    }
    asm volatile("" ::: "memory");
    return folded;
}
#endif

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (!request || !epycbench::module_detail::valid_u64_extrema_checksum(*request, checksum)) return 1;
    if (const auto status = prepare_avx2(request); status != 0) return status;
#if defined(__x86_64__)
    *checksum = epycbench_min_u64_avx2_body(request->data,
        static_cast<std::size_t>(request->bytes / 8U), request->repetitions);
    return 0;
#else
    return 2;
#endif
}
