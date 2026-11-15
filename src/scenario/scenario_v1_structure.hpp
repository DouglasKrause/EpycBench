#pragma once

#include "epycbench/scenario/structural_validation.hpp"

#include <vector>

namespace epycbench::scenario::detail {

[[nodiscard]] auto validate_v1_structure(const AuthoredScenario& authored)
    -> std::vector<ScenarioDiagnostic>;

[[nodiscard]] auto make_structure_internal_error(std::string classification) -> core::Error;

} // namespace epycbench::scenario::detail
