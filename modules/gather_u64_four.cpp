#include "gather_u64_module_support.hpp"

#if defined(__x86_64__)
#define EPYCBENCH_GATHER_TARGET gnu::target("sse2,no-avx"),
#else
#define EPYCBENCH_GATHER_TARGET
#endif
extern "C"[[ gnu::visibility("protected"), EPYCBENCH_GATHER_TARGET gnu::noinline ]] auto
epycbench_gather_u64_four_body(const unsigned char* values, const unsigned char* indices,
                               std::size_t value_count, std::size_t index_count,
                               std::uint64_t repetitions, std::uint64_t* result) noexcept -> int {
    std::uint64_t folded{};
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::uint64_t first{}, second{}, third{}, fourth{};
        std::size_t i{};
        for (; index_count - i >= 4U; i += 4U) {
            const auto a = epycbench::module_detail::load_sum_word(indices, i);
            const auto b = epycbench::module_detail::load_sum_word(indices, i + 1U);
            const auto c = epycbench::module_detail::load_sum_word(indices, i + 2U);
            const auto d = epycbench::module_detail::load_sum_word(indices, i + 3U);
            if (!epycbench::module_detail::gather_u64_index_valid(a, value_count) ||
                !epycbench::module_detail::gather_u64_index_valid(b, value_count) ||
                !epycbench::module_detail::gather_u64_index_valid(c, value_count) ||
                !epycbench::module_detail::gather_u64_index_valid(d, value_count))
                return 1;
            first += epycbench::module_detail::load_sum_word(values, static_cast<std::size_t>(a));
            second += epycbench::module_detail::load_sum_word(values, static_cast<std::size_t>(b));
            third += epycbench::module_detail::load_sum_word(values, static_cast<std::size_t>(c));
            fourth += epycbench::module_detail::load_sum_word(values, static_cast<std::size_t>(d));
        }
        for (; i < index_count; ++i) {
            const auto selected = epycbench::module_detail::load_sum_word(indices, i);
            if (!epycbench::module_detail::gather_u64_index_valid(selected, value_count))
                return 1;
            first +=
                epycbench::module_detail::load_sum_word(values, static_cast<std::size_t>(selected));
        }
        folded += (first + second) + (third + fourth);
    }
    asm volatile("" ::: "memory");
    *result = folded;
    return 0;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto
epycbench_kernel_execute_v2(const EpycBenchKernelRequestV2* request,
                            std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_gather_u64(request) != 0 ||
        !epycbench::module_detail::valid_gather_u64_checksum(*request, checksum))
        return 1;
    const auto values =
        static_cast<std::size_t>(epycbench::module_detail::load_sum_word(request->data, 1U));
    const auto indices =
        static_cast<std::size_t>(epycbench::module_detail::load_sum_word(request->data, 2U));
    std::uint64_t result{};
    const auto status =
        epycbench_gather_u64_four_body(request->data + 32U, request->data + 32U + 8U * values,
                                       values, indices, request->repetitions, &result);
    if (status == 0)
        *checksum = result;
    return status;
}
