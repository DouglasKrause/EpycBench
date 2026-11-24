#include "chase4_u64_module_support.hpp"

// Canonical content is a host precondition; no timed range/content scan.
// Serialized loop boundaries are compiler barriers, never hardware fences.
extern "C" [[gnu::visibility("protected"), gnu::noinline, gnu::target("sse2,no-avx")]]
auto epycbench_chase4_u64_serialized_body(const unsigned char* input, std::size_t nodes,
                                          std::uint64_t repetitions) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    const auto hops = static_cast<std::uint64_t>(nodes) + 1U;
    for (std::uint64_t repetition{}; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        std::size_t first{}, second = nodes, third = 2U * nodes, fourth = 3U * nodes;
        for (auto remaining = hops; remaining != 0U; --remaining) {
            std::uint64_t next;
            std::memcpy(&next, input + first * 64U, sizeof(next));
            first = static_cast<std::size_t>(next);
        }
        asm volatile("" ::: "memory");
        for (auto remaining = hops; remaining != 0U; --remaining) {
            std::uint64_t next;
            std::memcpy(&next, input + second * 64U, sizeof(next));
            second = static_cast<std::size_t>(next);
        }
        asm volatile("" ::: "memory");
        for (auto remaining = hops; remaining != 0U; --remaining) {
            std::uint64_t next;
            std::memcpy(&next, input + third * 64U, sizeof(next));
            third = static_cast<std::size_t>(next);
        }
        asm volatile("" ::: "memory");
        for (auto remaining = hops; remaining != 0U; --remaining) {
            std::uint64_t next;
            std::memcpy(&next, input + fourth * 64U, sizeof(next));
            fourth = static_cast<std::size_t>(next);
        }
        asm volatile("" ::: "memory");
        folded += static_cast<std::uint64_t>(first) + 3U * static_cast<std::uint64_t>(second) +
                  5U * static_cast<std::uint64_t>(third) + 7U * static_cast<std::uint64_t>(fourth);
    }
    asm volatile("" ::: "memory");
    return folded;
}
extern "C" [[gnu::visibility("default"), gnu::noinline]] auto
epycbench_kernel_execute_v2(const EpycBenchKernelRequestV2* request,
                            std::uint64_t* checksum) noexcept -> int {
    if (epycbench::module_detail::prepare_chase4_u64(request) != 0 ||
        !epycbench::module_detail::valid_chase4_u64_checksum(*request, checksum))
        return 1;
    *checksum = epycbench_chase4_u64_serialized_body(
        request->data, static_cast<std::size_t>(request->bytes / 256U), request->repetitions);
    return 0;
}
