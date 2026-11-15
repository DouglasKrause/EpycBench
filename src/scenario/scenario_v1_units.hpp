#pragma once

#include "epycbench/scenario/unit_normalization.hpp"

#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace epycbench::scenario::detail {

enum class QuantityFamily { duration, percentage };

struct UnitBinding {
    std::string_view path_pattern;
    QuantityFamily family;
    std::size_t traversal_rank;
};

[[nodiscard]] auto scenario_v1_unit_bindings() noexcept -> std::span<const UnitBinding>;

[[nodiscard]] auto normalize_v1_units(const StructurallyValidScenario& scenario,
                                      nlohmann::ordered_json& normalized)
    -> std::expected<std::vector<ScenarioDiagnostic>, core::Error>;

[[nodiscard]] auto make_units_internal_error(std::string classification) -> core::Error;

} // namespace epycbench::scenario::detail
