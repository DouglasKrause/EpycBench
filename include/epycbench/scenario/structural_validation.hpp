#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/scenario/authored_scenario.hpp"

#include <compare>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace epycbench::scenario {

struct ScenarioDiagnostic {
    core::StableErrorCode code;
    std::string json_pointer;
    std::optional<SourceLocation> location;
    std::map<std::string, std::string> safe_context;

    auto operator<=>(const ScenarioDiagnostic&) const = default;
};

struct StructuralValidationResult;

class StructurallyValidScenario {
  public:
    [[nodiscard]] auto authored() const noexcept -> const AuthoredScenario& { return authored_; }
    [[nodiscard]] auto value() const noexcept -> const nlohmann::ordered_json& {
        return authored_.value();
    }

  private:
    explicit StructurallyValidScenario(AuthoredScenario authored);

    AuthoredScenario authored_;

    friend auto validate_scenario_structure(AuthoredScenario)
        -> std::expected<StructuralValidationResult, core::Error>;
};

struct StructuralValidationResult {
    std::optional<StructurallyValidScenario> scenario;
    std::vector<ScenarioDiagnostic> diagnostics;
};

[[nodiscard]] auto validate_scenario_structure(AuthoredScenario authored)
    -> std::expected<StructuralValidationResult, core::Error>;

} // namespace epycbench::scenario
