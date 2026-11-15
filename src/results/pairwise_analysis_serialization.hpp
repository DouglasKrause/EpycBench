#pragma once
#include "epycbench/core/canonical_json.hpp"
#include "epycbench/results/pairwise_run_result.hpp"

namespace epycbench::results::analysis {
[[nodiscard]] auto serialize_pairwise_analysis(const PairwiseRunResult&, core::JcsLimits)
    -> std::expected<std::string, core::Error>;
[[nodiscard]] auto serialize_diagnostic_statistics(const PairwiseStatistics&,
    std::span<const runtime::PairwiseBlockAttemptEvidence>,
    const runtime::StatisticsProductionPolicy&, core::JcsLimits)
    -> std::expected<std::string, core::Error>;
} // namespace epycbench::results::analysis
