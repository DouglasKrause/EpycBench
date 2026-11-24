#pragma once

#include "epycbench/core/digest.hpp"

#include <nlohmann/json_fwd.hpp>
#include <span>

namespace epycbench::experiment {

struct ComparisonPoint {
    std::size_t ordinal;
    core::Sha256Digest candidate;
    core::Sha256Digest problem;
    core::Sha256Digest build;
    double oriented_ratio;
};

// One equally weighted finite positive ratio per Run. These are empirical
// distributions, separate from the ordinary within-Run confidence intervals.
[[nodiscard]] auto comparison_distributions(std::span<const ComparisonPoint>) -> nlohmann::json;

} // namespace epycbench::experiment
