#pragma once
#include "results/calculation_failure.hpp"
namespace epycbench::results::detail {
// Pure calculation over already admitted factual blocks. This grants no terminal
// or authoritative-result state. Caller admits ordinary storage/work first.
[[nodiscard]] auto
calculate_pairwise_statistics(std::span<const runtime::PairwiseBlockAttemptEvidence>,
                              const runtime::StatisticsProductionPolicy&)
    -> std::expected<PairwiseStatistics, CalculationError>;
} // namespace epycbench::results::detail
