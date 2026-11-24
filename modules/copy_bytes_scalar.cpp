#include "copy_bytes_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_COPY_SCALAR_TARGET gnu::target("sse2,no-avx"),
#else
#define EPYCBENCH_COPY_SCALAR_TARGET
#endif

extern "C" [[gnu::visibility("protected"), EPYCBENCH_COPY_SCALAR_TARGET gnu::noinline]]
auto epycbench_copy_bytes_scalar_body(const unsigned char* input, unsigned char* output,
    std::size_t count, std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        std::size_t offset{};
        for (; count - offset >= sizeof(std::uint64_t); offset += sizeof(std::uint64_t)) {
            std::uint64_t word;
            std::memcpy(&word, input + offset, sizeof(word));
            // Keep a scalar register boundary even when the compiler sees a copy loop.
            asm volatile("" : "+r"(word));
            std::memcpy(output + offset, &word, sizeof(word));
        }
        epycbench::module_detail::copy_byte_tail(input, output, offset, count);
        folded += epycbench::module_detail::copied_last_byte(output, count);
    }
    return folded;
}

extern "C" [[gnu::visibility("default"), gnu::noinline]] auto epycbench_kernel_execute_v3(
    const EpycBenchKernelRequestV3* request, std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_copy_bytes(request) != 0 ||
        !epycbench::module_detail::valid_xor_checksum(*request, checksum)) return 1;
    *checksum = epycbench_copy_bytes_scalar_body(request->input, request->output,
        static_cast<std::size_t>(request->input_bytes), request->repetitions);
    return 0;
}
