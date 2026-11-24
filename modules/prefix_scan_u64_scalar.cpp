#include "prefix_scan_u64_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_PREFIX_SCALAR_TARGET gnu::target("sse2,no-avx"),
#else
#define EPYCBENCH_PREFIX_SCALAR_TARGET
#endif

extern "C" [[gnu::visibility("protected"), EPYCBENCH_PREFIX_SCALAR_TARGET gnu::noinline]]
auto epycbench_prefix_scan_u64_scalar_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        // Make every repetition's reads and complete output stores observable
        // to the compiler, without inserting a CPU fence into the kernel.
        asm volatile("" ::: "memory");
        std::uint64_t prefix{}, answer{};
        for (std::size_t i{}; i < count; ++i) {
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
    *checksum = epycbench_prefix_scan_u64_scalar_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U), request->repetitions);
    return 0;
}
