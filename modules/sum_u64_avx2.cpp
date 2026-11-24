#include "task_module_support.hpp"

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace {
constexpr int unsupported_isa = 2;

auto prepare_avx2(const EpycBenchKernelRequestV2* request) noexcept -> int {
    if (const auto invalid = epycbench::module_detail::prepare_sum_u64(request); invalid != 0)
        return invalid;
#if defined(__x86_64__)
    if (__builtin_cpu_supports("avx2")) return 0;
#endif
    return unsupported_isa;
}
} // namespace

#if defined(__x86_64__)
// This symbol contains the actual repeated work inspected by the basic checker.
// The ABI callback below admits the ISA before entering it.
extern "C" [[gnu::visibility("protected"), gnu::target("avx2"), gnu::noinline]]
auto epycbench_sum_u64_avx2_body(const unsigned char* input, std::size_t count,
                               std::uint64_t repetitions) noexcept -> std::uint64_t {
    static_assert(std::endian::native == std::endian::little);
    constexpr std::size_t lanes = 4;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        auto accumulators = _mm256_setzero_si256();
        std::size_t index{};
        for (; count - index >= lanes; index += lanes) {
            const auto* words = input + index * sizeof(std::uint64_t);
            const auto values = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(words));
            accumulators = _mm256_add_epi64(accumulators, values);
        }
        const auto pairs = _mm_add_epi64(_mm256_castsi256_si128(accumulators),
                                         _mm256_extracti128_si256(accumulators, 1));
        auto sum = static_cast<std::uint64_t>(_mm_cvtsi128_si64(pairs)) +
                   static_cast<std::uint64_t>(_mm_extract_epi64(pairs, 1));
        for (; index < count; ++index)
            sum += epycbench::module_detail::load_sum_word(input, index);
        folded += sum;
    }
    asm volatile("" ::: "memory");
    return folded;
}
#endif

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (!checksum) return 1;
    if (const auto status = prepare_avx2(request); status != 0) return status;
#if defined(__x86_64__)
    const auto count = static_cast<std::size_t>(request->bytes / sizeof(std::uint64_t));
    *checksum = epycbench_sum_u64_avx2_body(request->data, count, request->repetitions);
    return 0;
#else
    return unsupported_isa;
#endif
}
