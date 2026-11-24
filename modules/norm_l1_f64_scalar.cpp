#include "dyadic_f64_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_NORM_L1_SCALAR_TARGET gnu::target("sse2,no-avx,no-fma"),
#else
#define EPYCBENCH_NORM_L1_SCALAR_TARGET
#endif
extern "C" [[gnu::visibility("protected"), EPYCBENCH_NORM_L1_SCALAR_TARGET gnu::noinline]]
auto epycbench_norm_l1_f64_scalar_body(const unsigned char* input, std::size_t elements,
    std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        double sum = 0.0;
        for (std::size_t index{}; index < elements; ++index)
            sum += epycbench::module_detail::load_dyadic_f64_magnitude(input, index);
        folded += epycbench::module_detail::canonical_dyadic_f64_bits(sum);
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v2(
    const EpycBenchKernelRequestV2* request, std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_dyadic_f64(request) != 0 ||
        !epycbench::module_detail::valid_dyadic_f64_checksum(*request, checksum)) return 1;
    *checksum = epycbench_norm_l1_f64_scalar_body(request->data,
        static_cast<std::size_t>(request->bytes / 8U), request->repetitions);
    return 0;
}
