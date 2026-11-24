#include "epycbench/kernel/convolution_u64_task.hpp"
#include "output_task_module_support.hpp"

#include <array>

namespace {
using namespace epycbench;
constexpr auto taps = kernel::convolution_u64_taps.size();
constexpr auto header = kernel::convolution_u64_header_words;

auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    using namespace module_detail;
    if (!request || !request->input || !request->output || request->input_bytes < 8U * header ||
        request->xor_mask || !request->repetitions ||
        load_sum_word(request->input, 0U) != kernel::convolution_u64_input_magic)
        return 1;
    const auto n = load_sum_word(request->input, 1U);
    if (n < taps || n > kernel::convolution_u64_maximum_samples)
        return 1;
    const auto outputs = n - taps + 1U;
    return request->input_bytes != 8U * (header + n) || request->output_bytes != 8U * outputs ||
           request->repetitions > UINT64_MAX / (outputs * taps) ||
           byte_ranges_overlap(request->input, request->input_bytes, request->output,
                               request->output_bytes);
}

template <std::size_t Lanes>
[[gnu::always_inline]] inline auto convolve(const unsigned char* input, unsigned char* output,
                                            std::size_t first) noexcept -> std::uint64_t {
    std::array<std::uint64_t, Lanes> sums{};
    for (std::size_t k = 0; k < taps; ++k) {
        const auto coefficient = module_detail::load_sum_word(input, 2U + k);
        for (std::size_t lane = 0; lane < Lanes; ++lane)
            sums[lane] += coefficient * module_detail::load_sum_word(input, header + first + lane +
                                                                                taps - 1U - k);
    }
    std::uint64_t folded{};
    for (std::size_t lane = 0; lane < Lanes; ++lane) {
        module_detail::store_xor_word(output, first + lane, sums[lane]);
        folded += sums[lane];
    }
    return folded;
}
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_convolution_u64_body(const unsigned char* input, unsigned char* output,
                                    std::size_t outputs, std::uint64_t repetitions) noexcept
    -> std::uint64_t {
    std::uint64_t sum{};
    for (std::uint64_t r = 0; r < repetitions; ++r) {
        asm volatile("" ::: "memory");
        std::size_t i{};
#if EPYCBENCH_CONVOLUTION_BLOCK4
        for (; outputs - i >= 4U; i += 4U)
            sum += convolve<4U>(input, output, i);
#endif
        for (; i < outputs; ++i)
            sum += convolve<1U>(input, output, i);
    }
    asm volatile("" ::: "memory");
    return sum;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                                 std::uint64_t* answer) noexcept -> int {
    if (prepare(request) || !epycbench::module_detail::valid_xor_checksum(*request, answer))
        return 1;
    *answer = epycbench_convolution_u64_body(request->input, request->output,
                                             request->output_bytes / 8U, request->repetitions);
    return 0;
}
