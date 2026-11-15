#pragma once

#include "epycbench/runtime/pairwise_run_evidence.hpp"

#include <nlohmann/json_fwd.hpp>

namespace epycbench::runtime::wire_v1::detail {

struct ComponentProjectionCount;
[[nodiscard]] auto count_region_finalization_diagnostic(
    const memory::RegionFinalizationDiagnostic&) -> ComponentProjectionCount;
[[nodiscard]] auto project_region_finalization_diagnostic(
    const memory::RegionFinalizationDiagnostic&) -> nlohmann::json;
[[nodiscard]] auto count_pairwise_finalization_evidence(
    const PairwiseFinalizationEvidence&) -> ComponentProjectionCount;

[[nodiscard]] auto project_pairwise_finalization_evidence(
    const PairwiseFinalizationEvidence& value) -> nlohmann::json;

} // namespace epycbench::runtime::wire_v1::detail
