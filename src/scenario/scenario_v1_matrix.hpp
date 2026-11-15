#pragma once

#include "epycbench/scenario/matrix_expansion.hpp"

namespace epycbench::scenario::detail {

struct MatrixExpansionPayload {
    std::vector<ExpandedMatrixExperiment> experiments;
    std::vector<ResolvedComparisonExperiment> comparisons;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto expand_v1_matrices(const KernelDispatchedScenario& scenario,
                                      const ResourceResolutionContext& context,
                                      core::ByteCount base_page_size)
    -> std::expected<MatrixExpansionPayload, core::Error>;
[[nodiscard]] auto expand_v1_matrices(const KernelDispatchedScenario& scenario,
                                      const ResourceResolutionContext& context,
                                      core::ByteCount base_page_size,
                                      MatrixExpansionLimits limits,
                                      std::size_t* charged_bytes = nullptr)
    -> std::expected<MatrixExpansionPayload, core::Error>;
[[nodiscard]] auto matrix_internal_error(std::string classification, std::string underlying = {})
    -> core::Error;

} // namespace epycbench::scenario::detail
