#pragma once

#include "epycbench/scenario/memory_placement_resolution.hpp"

namespace epycbench::scenario::detail {

struct MemoryPlacementPayload {
    std::vector<ComparisonVariantMemoryPlacement> comparison;
    std::vector<MatrixMemoryPlacementTemplate> matrix_templates;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto resolve_v1_memory_placement(const ServiceCpuResolvedScenario& scenario,
                                               const ResourceResolutionContext& context,
                                               core::ByteCount base_page_size)
    -> std::expected<MemoryPlacementPayload, core::Error>;
[[nodiscard]] auto memory_internal_error(std::string classification,
                                         std::string underlying = {}) -> core::Error;

} // namespace epycbench::scenario::detail
