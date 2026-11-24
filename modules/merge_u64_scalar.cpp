#include "merge_u64_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_MERGE_SCALAR_TARGET gnu::target("sse2,no-avx"),
#else
#define EPYCBENCH_MERGE_SCALAR_TARGET
#endif

extern "C" [[gnu::visibility("protected"), EPYCBENCH_MERGE_SCALAR_TARGET gnu::noinline]]
auto epycbench_merge_u64_scalar_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        // Every repetition must reload both runs and overwrite the full output.
        asm volatile("" ::: "memory");
        const auto midpoint = count / 2U;
        std::size_t left{}, right = midpoint, position{};
        std::uint64_t answer{};
        const auto emit = [&](std::uint64_t word) {
            store_xor_word(output, position, word);
            answer += (position + 1U) * word;
            ++position;
        };
        while (left < midpoint && right < count) {
            const auto a = load_sum_word(input, left), b = load_sum_word(input, right);
            if (a <= b) { emit(a); ++left; }
            else { emit(b); ++right; }
        }
        while (left < midpoint) emit(load_sum_word(input, left++));
        while (right < count) emit(load_sum_word(input, right++));
        folded += answer;
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_merge_u64(request) != 0 ||
        !epycbench::module_detail::valid_xor_checksum(*request, checksum)) return 1;
    *checksum = epycbench_merge_u64_scalar_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U), request->repetitions);
    return 0;
}
