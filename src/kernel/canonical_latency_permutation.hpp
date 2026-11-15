#pragma once

#include "bounded_index.hpp"
#include "mix64.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

namespace epycbench::kernel::detail {

enum class CanonicalLatencyPermutationFailure {
    stream_ordinal_exhausted,
};

[[nodiscard]] inline auto canonical_latency_permutation(
    std::size_t node_count, std::uint64_t resolved_seed)
    -> std::expected<std::vector<std::uint64_t>,
                     CanonicalLatencyPermutationFailure> {
    std::vector<std::uint64_t> permutation(node_count);
    std::iota(permutation.begin(), permutation.end(), UINT64_C(0));
    std::uint64_t ordinal = 0U;
    for (std::size_t i = node_count - 1U; i > 0U; --i) {
        const auto bound = static_cast<std::uint64_t>(i) + 1U;
        std::optional<std::uint64_t> selected;
        for (;;) {
            if (ordinal == std::numeric_limits<std::uint64_t>::max()) {
                return std::unexpected{
                    CanonicalLatencyPermutationFailure::stream_ordinal_exhausted};
            }
            const auto draw = mix64_v1(resolved_seed + ordinal);
            ++ordinal;
            selected = bounded_index(draw, bound);
            if (selected) {
                break;
            }
        }
        std::swap(permutation[i], permutation[static_cast<std::size_t>(*selected)]);
    }
    return permutation;
}

} // namespace epycbench::kernel::detail
