#include "epycbench/kernel/histogram_u64_task.hpp"
#include "output_task_module_support.hpp"
#include <array>
#include <new>

namespace {
using namespace epycbench;
constexpr std::uint64_t bins = kernel::histogram_u64_bins;
constexpr std::uint64_t weight_mask = (UINT64_C(1) << 56U) - 1U;

auto prepare(const EpycBenchKernelRequestV3* request) noexcept -> int {
    using namespace module_detail;
    if (!request || !request->input || !request->output || request->xor_mask ||
        !request->repetitions || request->input_bytes < 16U ||
        request->output_bytes != bins * 16U ||
        load_sum_word(request->input, 0) != kernel::histogram_u64_input_magic)
        return 1;
    const auto n = load_sum_word(request->input, 1U);
    return !n || n > kernel::histogram_u64_maximum_records ||
           request->input_bytes != 8U * (n + 2U) ||
           request->repetitions > UINT64_MAX / (n > bins ? n : bins) ||
           byte_ranges_overlap(request->input, request->input_bytes, request->output,
                               request->output_bytes);
}
#if EPYCBENCH_HISTOGRAM_HASH
struct Entry {
    std::uint64_t key, count, weight;
};
using Table = std::array<Entry, kernel::histogram_u64_hash_slots>;
static_assert(sizeof(Table) == 12288U);
#endif
} // namespace

extern "C" [[gnu::visibility("protected"), gnu::noinline]]
auto epycbench_histogram_u64_body(const unsigned char* input, unsigned char* output,
                                  std::size_t records, std::uint64_t repetitions
#if EPYCBENCH_HISTOGRAM_HASH
                                  , Table* workspace
#endif
                                  ) noexcept
    -> std::uint64_t {
    using namespace epycbench::module_detail;
    std::uint64_t folded{};
    for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition) {
        asm volatile("" ::: "memory");
        for (std::size_t k = 0; k < 2U * bins; ++k)
            store_xor_word(output, k, 0U);
#if EPYCBENCH_HISTOGRAM_HASH
        auto& table = *workspace;
        for (auto& entry : table)
            entry = {bins, 0U, 0U};
#endif
        for (std::size_t i = 0; i < records; ++i) {
            const auto word = load_sum_word(input, 2U + i), key = word >> 56U,
                       weight = word & weight_mask;
#if EPYCBENCH_HISTOGRAM_HASH
            auto index = ((key * 40503U) & 65535U) >> 7U;
            // At most 256 distinct keys occupy this 512-slot table.
            while (table[index].key != bins && table[index].key != key)
                index = (index + 1U) % table.size();
            auto& entry = table[index];
            entry.key = key;
            ++entry.count;
            entry.weight += weight;
#else
            store_xor_word(output, 2U * key, load_sum_word(output, 2U * key) + 1U);
            store_xor_word(output, 2U * key + 1U, load_sum_word(output, 2U * key + 1U) + weight);
#endif
        }
#if EPYCBENCH_HISTOGRAM_HASH
        for (const auto& entry : table)
            if (entry.key != bins) {
                store_xor_word(output, 2U * entry.key, entry.count);
                store_xor_word(output, 2U * entry.key + 1U, entry.weight);
            }
#endif
        for (std::size_t k = 0; k < bins; ++k)
            folded +=
                (k + 1U) * (load_sum_word(output, 2U * k) + load_sum_word(output, 2U * k + 1U));
    }
    asm volatile("" ::: "memory");
    return folded;
}

#if EPYCBENCH_HISTOGRAM_HASH
extern "C" [[gnu::visibility("hidden"), gnu::noinline]]
auto epycbench_histogram_u64_execute_package(const EpycBenchKernelRequestV3* request,
    std::uint64_t* answer, unsigned char* scratch, std::uint64_t scratch_bytes) noexcept -> int {
    using namespace epycbench::module_detail;
    if (prepare(request) || !valid_xor_checksum(*request, answer) || !scratch ||
        scratch_bytes != sizeof(Table) || reinterpret_cast<std::uintptr_t>(scratch) % alignof(Table) != 0)
        return 1;
    auto* table = ::new (scratch) Table;
    *answer = epycbench_histogram_u64_body(request->input, request->output,
        load_sum_word(request->input, 1U), request->repetitions, table);
    return 0;
}
#else
extern "C" [[gnu::visibility("default"), gnu::noinline]]
auto epycbench_kernel_execute_v3(const EpycBenchKernelRequestV3* request,
                                 std::uint64_t* answer) noexcept -> int {
    using namespace epycbench::module_detail;
    if (prepare(request) || !valid_xor_checksum(*request, answer))
        return 1;
    *answer = epycbench_histogram_u64_body(request->input, request->output,
                                           load_sum_word(request->input, 1U), request->repetitions);
    return 0;
}
#endif
