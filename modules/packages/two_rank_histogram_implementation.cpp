#include "epycbench/kernel/package.h"

#include "kernel/histogram_u64_leaf.hpp"

namespace {
namespace kernel = epycbench::kernel;
namespace leaf = kernel::detail::histogram_leaf;
constexpr bool private_partials = EPYCBENCH_HISTOGRAM_PRIVATE == 1;
constexpr std::size_t receipt_bytes = 4U * sizeof(std::uint64_t);
} // namespace

extern "C" [[gnu::visibility("default")]] auto epycbench_package_histogram_rank_v1(
    const EpycBenchPackageCallV1* call, EpycBenchPackageWorkV1* work) noexcept -> int {
    const auto& request = *call->task;
    if (request.rank >= 2U || request.rank_count != 2U || request.repetitions != 1U ||
        call->shared_input_count != 1U || call->shared_count != 3U ||
        call->shared[0].bytes != kernel::histogram_u64_scratch_bytes) return 1;
    const leaf::Input input{reinterpret_cast<const std::byte*>(call->shared_inputs[0].data), call->shared_inputs[0].bytes};
    if (input.size() < 8U * kernel::histogram_u64_header_words) return 1;
    const auto record_count = (input.size() - 8U * kernel::histogram_u64_header_words) / 8U;
    if (request.partition_begin > record_count || request.partition_count > record_count - request.partition_begin) return 1;
    const auto records = input.subspan(8U * (kernel::histogram_u64_header_words + request.partition_begin),
                                       8U * request.partition_count);
    auto& scratch = *reinterpret_cast<kernel::HistogramU64Scratch*>(call->shared[0].data);
    auto& slice = scratch.ranks[private_partials ? request.rank : 0U];
    leaf::accumulate<!private_partials>(records, slice, request.partition_count);
    *work = {1, request.partition_count, 0};
    return 0;
}

extern "C" [[gnu::visibility("default")]] auto epycbench_package_histogram_finalize_v1(
    const EpycBenchPackageCallV1* call, const EpycBenchPackageRankReceiptV1* ranks, std::uint32_t rank_count,
    EpycBenchPackageWorkV1* work) noexcept -> int {
    if (rank_count != 2 || call->shared_count != 3 || call->shared[0].bytes != kernel::histogram_u64_scratch_bytes ||
        call->shared[1].bytes != 16U * kernel::histogram_u64_bins || call->shared[2].bytes != receipt_bytes)
        return 1;
    const auto& scratch = *reinterpret_cast<const kernel::HistogramU64Scratch*>(call->shared[0].data);
    const auto checksum = leaf::materialize<private_partials>(scratch,
        {reinterpret_cast<std::byte*>(call->shared[1].data), call->shared[1].bytes});
    const auto consumed = ranks[0].actual.work_units + ranks[1].actual.work_units;
    const leaf::Output receipt{reinterpret_cast<std::byte*>(call->shared[2].data), call->shared[2].bytes};
    leaf::write(receipt, 0, checksum);
    leaf::write(receipt, 1, private_partials ? 0 : 2U * consumed);
    leaf::write(receipt, 2, private_partials ? 2U * consumed : 0);
    leaf::write(receipt, 3, private_partials ? kernel::histogram_u64_bins : 0);
    *work = {1, consumed, kernel::histogram_u64_bins};
    return 0;
}

extern "C" [[gnu::visibility("default")]] auto epycbench_package_implementation_v1() noexcept
    -> const EpycBenchPackageImplementationV1* {
    static constexpr EpycBenchPackageImplementationV1 descriptor{EPYCBENCH_PACKAGE_ABI_VERSION,
        sizeof(EpycBenchPackageImplementationV1), epycbench_package_histogram_rank_v1,
        epycbench_package_histogram_finalize_v1};
    return &descriptor;
}
