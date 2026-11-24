#include "varint14_module_support.hpp"

// Equivalent numeric parsing with a different physical load strategy. The
// main loop reads the next byte even for a one-byte record, then masks it out.
// Its pointer bound keeps both loads within the logical extent. Loop/terminal
// control remains; this is not an isolated measurement of predictor cost.
extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_varint14_masked_body(const unsigned char* input, std::size_t bytes,
                                    std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        const auto* cursor = input;
        const auto* const last = input + bytes - 1U;
        std::uint64_t answer{};
        while (cursor < last) {
            const std::uint64_t lead = cursor[0];
            const std::uint64_t lookahead = cursor[1];
            const std::uint64_t wide = lead >> 7U;
            const std::uint64_t mask = std::uint64_t{0} - wide;
            answer += (lead & 127U) | ((lookahead << 7U) & mask);
            cursor += static_cast<std::size_t>(wide) + 1U;
        }
        // prepare_varint14 requires a terminating final byte. If the last
        // record was wide, cursor is already one-past-end instead.
        if (cursor == last)
            answer += *cursor;
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
    *checksum = epycbench_varint14_masked_body(
        request->data, static_cast<std::size_t>(request->bytes), request->repetitions);
    return 0;
}
