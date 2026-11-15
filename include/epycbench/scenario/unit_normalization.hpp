#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/scenario/structural_validation.hpp"

#include <expected>
#include <optional>
#include <vector>

namespace epycbench::scenario {

struct UnitNormalizationResult;

class UnitNormalizedScenario {
  public:
    UnitNormalizedScenario(const UnitNormalizedScenario&);
    auto operator=(const UnitNormalizedScenario&) -> UnitNormalizedScenario&;
    UnitNormalizedScenario(UnitNormalizedScenario&&) noexcept = default;
    auto operator=(UnitNormalizedScenario&&) noexcept -> UnitNormalizedScenario&;
    ~UnitNormalizedScenario();
    [[nodiscard]] auto structurally_valid() const noexcept -> const StructurallyValidScenario& {
        return scenario_;
    }

    [[nodiscard]] auto value() const noexcept -> const nlohmann::ordered_json& {
        return normalized_value_;
    }

  private:
    UnitNormalizedScenario(StructurallyValidScenario scenario,
                           nlohmann::ordered_json normalized_value);

    StructurallyValidScenario scenario_;
    nlohmann::ordered_json normalized_value_;

    friend auto normalize_scenario_units(StructurallyValidScenario)
        -> std::expected<UnitNormalizationResult, core::Error>;
};

struct UnitNormalizationResult {
    std::optional<UnitNormalizedScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto normalize_scenario_units(StructurallyValidScenario scenario)
    -> std::expected<UnitNormalizationResult, core::Error>;

} // namespace epycbench::scenario
