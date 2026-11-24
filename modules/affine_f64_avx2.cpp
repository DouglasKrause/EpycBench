#include "affine_f64_module_support.hpp"
#include <array>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
auto prepare_avx2(const EpycBenchKernelRequestV3* request) noexcept -> int {
    if (epycbench::module_detail::prepare_affine_f64(request) != 0) return 1;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) return 0;
#endif
    return 2;
}
} // namespace

#if defined(__x86_64__)
extern "C" [[gnu::visibility("protected"), gnu::target("avx2,no-fma"), gnu::noinline]]
auto epycbench_affine32_f64_avx2_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    static_assert(std::endian::native == std::endian::little);
    const auto half = _mm256_set1_pd(0.5);
    const auto quarter = _mm256_set1_pd(0.25);
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        auto accumulator = _mm256_setzero_si256();
        std::size_t index{};
        for (; count - index >= 4U; index += 4U) {
            __m256d value;
            std::memcpy(&value, input + index * 8U, sizeof(value));
            for (unsigned step{}; step < 32U; ++step)
                value = _mm256_add_pd(_mm256_mul_pd(value, half), quarter);
            std::memcpy(output + index * 8U, &value, sizeof(value));
            accumulator = _mm256_add_epi64(accumulator, _mm256_castpd_si256(value));
        }
        std::array<std::uint64_t, 4> words{};
        std::memcpy(words.data(), &accumulator, sizeof(accumulator));
        std::uint64_t sum = words[0] + words[1] + words[2] + words[3];
        for (; index < count; ++index) {
            auto value = epycbench::module_detail::affine_f64_value(input, index);
            for (unsigned step{}; step < 32U; ++step) value = value * 0.5 + 0.25;
            const auto bits = std::bit_cast<std::uint64_t>(value);
            epycbench::module_detail::store_affine_f64(output, index, bits);
            sum += bits;
        }
        folded += sum;
    }
    asm volatile("" ::: "memory");
    return folded;
}
#endif

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    if (!request || !epycbench::module_detail::valid_xor_checksum(*request, checksum)) return 1;
    if (const auto status = prepare_avx2(request); status != 0) return status;
#if defined(__x86_64__)
    *checksum = epycbench_affine32_f64_avx2_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U), request->repetitions);
    return 0;
#else
    return 2;
#endif
}
