#include "u64_extrema_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_MAX_SCALAR_TARGET gnu::target("sse2,no-avx"),
#else
#define EPYCBENCH_MAX_SCALAR_TARGET
#endif
extern "C" [[gnu::visibility("protected"), EPYCBENCH_MAX_SCALAR_TARGET gnu::noinline]]
auto epycbench_max_u64_scalar_body(const unsigned char* input, std::size_t count,
    std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t maximum{};
        for (std::size_t index{}; index < count; ++index) {
            const auto word = epycbench::module_detail::load_sum_word(input, index);
            maximum = word > maximum ? word : maximum;
        }
        folded += maximum;
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_sum_u64(request) != 0 ||
        !epycbench::module_detail::valid_u64_extrema_checksum(*request, checksum)) return 1;
    *checksum = epycbench_max_u64_scalar_body(request->data,
        static_cast<std::size_t>(request->bytes / 8U), request->repetitions);
    return 0;
}
