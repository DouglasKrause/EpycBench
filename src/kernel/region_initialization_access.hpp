#pragma once

#include "epycbench/kernel/region_initialization.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace epycbench::kernel::detail {

struct RegionInitializationAccess {
    [[nodiscard]] static auto mix64(std::uint64_t value) noexcept -> std::uint64_t;
    static void store_little_endian(std::span<std::byte, 8> destination,
                                    std::uint64_t value) noexcept;
    [[nodiscard]] static auto canonical_permutation(std::size_t node_count,
                                                    std::uint64_t seed) noexcept
        -> std::expected<std::vector<std::uint64_t>, core::Error>;
    [[nodiscard]] static auto bounded_draw_for_test(std::span<const std::uint64_t> draws,
                                                    std::size_t& cursor,
                                                    std::uint64_t bound) noexcept
        -> std::expected<std::uint64_t, core::Error>;
    [[nodiscard]] static auto successors(const PreparedRegionInitializer& initializer) noexcept
        -> const std::vector<std::uint64_t>& {
        return initializer.successors_;
    }
    [[nodiscard]] static auto unchecked_spec(RegionContentFamily family,
                                             core::ByteCount logical_bytes,
                                             std::optional<std::uint64_t> seed) noexcept
        -> RegionInitializationSpec {
        return {family, logical_bytes, seed};
    }
    [[nodiscard]] static auto evidence(
        RegionContentFamily family, std::string_view layout_profile,
        core::ByteCount logical_bytes, std::optional<std::uint64_t> word_count,
        std::optional<std::uint64_t> node_count,
        std::optional<std::uint64_t> permutation_seed) noexcept -> RegionContentEvidence {
        return RegionContentEvidence{family, layout_profile, logical_bytes, word_count,
                                     node_count, permutation_seed, true};
    }
};

} // namespace epycbench::kernel::detail
