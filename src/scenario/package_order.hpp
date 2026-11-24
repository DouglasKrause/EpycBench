#pragma once

#include "epycbench/core/seed.hpp"

namespace epycbench::scenario {
// Each consecutive pair contains both orders. A fresh order seed can change
// the assignment independently in each pair while preserving this balance.
inline auto package_candidate_first(std::uint64_t seed, std::uint64_t block)
    -> std::expected<bool, core::Error> {
    auto order = core::derive_seed(
        seed, {core::SeedDomain::block_order, {{"block_pair", std::to_string(block / 2U)}}});
    if (!order)
        return std::unexpected{order.error()};
    return ((order->value() & 1U) != (block & 1U));
}
} // namespace epycbench::scenario
