#include "affine_f64_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_AFFINE_SCALAR_TARGET gnu::target("sse2,no-avx,no-fma"),
#else
#define EPYCBENCH_AFFINE_SCALAR_TARGET
#endif

extern "C" [[gnu::visibility("protected"), EPYCBENCH_AFFINE_SCALAR_TARGET gnu::noinline]]
auto epycbench_affine32_f64_scalar_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t sum{};
        for (std::size_t index{}; index < count; ++index) {
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

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_affine_f64(request) != 0 ||
        !epycbench::module_detail::valid_xor_checksum(*request, checksum)) return 1;
    *checksum = epycbench_affine32_f64_scalar_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes / 8U), request->repetitions);
    return 0;
}
