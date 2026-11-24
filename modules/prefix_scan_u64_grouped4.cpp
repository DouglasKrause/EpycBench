#include "prefix_scan_u64_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_PREFIX_SCALAR_TARGET gnu::target("sse2,no-avx"),
#else
#define EPYCBENCH_PREFIX_SCALAR_TARGET
#endif

extern "C" [[gnu::visibility("protected"), EPYCBENCH_PREFIX_SCALAR_TARGET gnu::noinline]]
auto epycbench_prefix_scan_u64_grouped4_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        // Make every repetition's reads and complete output stores observable
        // to the compiler, without inserting a CPU fence into the kernel.
        asm volatile("" ::: "memory");
        std::uint64_t prefix{}, answer{};
        std::size_t i{};
        for (; count - i >= 4U; i += 4U) {
            const auto a = load_sum_word(input, i), b = load_sum_word(input, i + 1U),
                       c = load_sum_word(input, i + 2U), d = load_sum_word(input, i + 3U);
            // Local prefixes form a four-element tree, independently of the
            // previous group's carry. Only the group total advances that carry.
            const auto ab = a + b, cd = c + d;
            auto local0 = a, local1 = ab, local2 = ab + c, local3 = ab + cd;
            // Preserve the local-prefix/carry boundary against unsigned-add
            // reassociation back into one element-by-element carry chain.
            // This register boundary emits no instruction or CPU fence.
            asm volatile("" : "+r"(local0), "+r"(local1), "+r"(local2), "+r"(local3));
            const auto p0 = prefix + local0, p1 = prefix + local1,
                       p2 = prefix + local2, p3 = prefix + local3;
            store_xor_word(output, i, p0); store_xor_word(output, i + 1U, p1);
            store_xor_word(output, i + 2U, p2); store_xor_word(output, i + 3U, p3);
            answer += p0 + p1 + p2 + p3;
            prefix = p3;
        }
        for (; i < count; ++i) {
            prefix += load_sum_word(input, i);
            store_xor_word(output, i, prefix);
            answer += prefix;
        }
        folded += answer;
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_prefix_scan_u64(request) != 0 ||
        !epycbench::module_detail::valid_xor_checksum(*request, checksum)) return 1;
    *checksum = epycbench_prefix_scan_u64_grouped4_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U), request->repetitions);
    return 0;
}
