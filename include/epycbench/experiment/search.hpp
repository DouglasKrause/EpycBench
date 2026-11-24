#pragma once

#include "epycbench/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace epycbench::experiment {

enum class SearchStrategy { enumeration, seeded_random };

[[nodiscard]] auto strategy_token(SearchStrategy) noexcept -> std::string_view;

// Visit every source ordinal at most once, independent of module availability
// or resolution outcomes. The caller retains the inspected prefix.
[[nodiscard]] auto candidate_order(SearchStrategy, std::size_t candidate_count, std::uint64_t seed)
    -> std::expected<std::vector<std::size_t>, core::Error>;

} // namespace epycbench::experiment
