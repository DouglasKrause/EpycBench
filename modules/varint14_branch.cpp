#include "varint14_module_support.hpp"

// Parse one immutable canonical stream per repetition. Variable-width memory
// consumption is part of the numeric algorithm; no ISA-specific instructions.
extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_varint14_branch_body(const unsigned char* input, std::size_t bytes,
                                    std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        const auto* cursor = input;
        const auto* const end = input + bytes;
        std::uint64_t answer{};
        while (cursor != end) {
            std::uint64_t value = *cursor++;
            if (value & 128U) {
                const std::uint64_t high = *cursor++;
                value = (value & 127U) | (high << 7U);
            }
            answer += value;
        }
        folded += answer;
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v2(const EpycBenchKernelRequestV2* request,
                                 std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_varint14(request) != 0 ||
        !epycbench::module_detail::valid_varint14_checksum(*request, checksum))
        return 1;
    *checksum = epycbench_varint14_branch_body(
        request->data, static_cast<std::size_t>(request->bytes), request->repetitions);
    return 0;
}
