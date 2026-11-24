#pragma once

#include "epycbench/kernel/two_rank_histogram_u64.hpp"

#include <array>
#include <atomic>

namespace epycbench::kernel {
struct alignas(histogram_u64_scratch_alignment) HistogramU64RankBins {
    struct Bin {
        std::uint64_t count{}, sum{};
    };
    std::array<Bin, histogram_u64_bins> bins;
};

struct HistogramU64Scratch {
    std::array<HistogramU64RankBins, histogram_u64_rank_count> ranks;
};

namespace detail::histogram_leaf {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
using Atomic = std::atomic_ref<std::uint64_t>;
constexpr std::uint64_t weight_mask = (UINT64_C(1) << 56U) - 1U;

static_assert(Atomic::is_always_lock_free && Atomic::required_alignment <= alignof(std::uint64_t));
static_assert(sizeof(HistogramU64RankBins) == 16U * histogram_u64_bins);
static_assert(sizeof(HistogramU64Scratch) == histogram_u64_scratch_bytes);
static_assert(alignof(HistogramU64Scratch) == histogram_u64_scratch_alignment);

inline auto read(Input bytes, std::size_t index) noexcept -> std::uint64_t {
    std::uint64_t value{};
    for (unsigned i{}; i < 8U; ++i)
        value |= std::uint64_t{std::to_integer<unsigned char>(bytes[8U * index + i])} << (8U * i);
    return value;
}

inline void write(Output bytes, std::size_t index, std::uint64_t value) noexcept {
    for (unsigned i{}; i < 8U; ++i)
        bytes[8U * index + i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
}

template <bool shared>
void accumulate(Input records, HistogramU64RankBins& destination, std::uint64_t count) noexcept {
    for (std::size_t i{}; i < count; ++i) {
        const auto word = read(records, i);
        auto& bin = destination.bins[word >> 56U];
        if constexpr (shared) {
            Atomic{bin.count}.fetch_add(1U, std::memory_order_relaxed);
            Atomic{bin.sum}.fetch_add(word & weight_mask, std::memory_order_relaxed);
        } else {
            ++bin.count;
            bin.sum += word & weight_mask;
        }
    }
}

template <bool combine>
auto materialize(const HistogramU64Scratch& scratch, Output output) noexcept -> std::uint64_t {
    std::uint64_t folded{};
    for (std::size_t key{}; key < histogram_u64_bins; ++key) {
        auto bin = scratch.ranks[0].bins[key];
        if constexpr (combine) {
            const auto& partial = scratch.ranks[1].bins[key];
            bin.count += partial.count;
            bin.sum += partial.sum;
        }
        write(output, 2U * key, bin.count);
        write(output, 2U * key + 1U, bin.sum);
        folded += (key + 1U) * (bin.count + bin.sum);
    }
    return folded;
}
} // namespace detail::histogram_leaf
} // namespace epycbench::kernel
