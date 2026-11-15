#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/scenario/unit_normalization.hpp"

#include <compare>
#include <expected>
#include <map>
#include <optional>
#include <string>

namespace epycbench::scenario {

enum class ValueOriginKind {
    explicit_value,
    schema_default,
    scenario_default,
    experiment_override,
    variant_override,
    matrix_override,
};

struct ValueOrigin {
    ValueOriginKind kind;
    std::optional<std::string> source_pointer{};

    auto operator<=>(const ValueOrigin&) const = default;
};

class DefaultExpandedScenario {
  public:
    DefaultExpandedScenario(const DefaultExpandedScenario&);
    auto operator=(const DefaultExpandedScenario&) -> DefaultExpandedScenario&;
    DefaultExpandedScenario(DefaultExpandedScenario&&) noexcept = default;
    auto operator=(DefaultExpandedScenario&&) noexcept -> DefaultExpandedScenario&;
    ~DefaultExpandedScenario();
    [[nodiscard]] auto unit_normalized() const noexcept -> const UnitNormalizedScenario& {
        return scenario_;
    }

    [[nodiscard]] auto value() const noexcept -> const nlohmann::ordered_json& {
        return expanded_value_;
    }

    [[nodiscard]] auto origins() const noexcept -> const std::map<std::string, ValueOrigin>& {
        return origins_;
    }

  private:
    DefaultExpandedScenario(UnitNormalizedScenario scenario, nlohmann::ordered_json expanded_value,
                            std::map<std::string, ValueOrigin> origins);

    UnitNormalizedScenario scenario_;
    std::map<std::string, ValueOrigin> origins_;
    nlohmann::ordered_json expanded_value_;

    friend auto expand_scenario_defaults(UnitNormalizedScenario)
        -> std::expected<DefaultExpandedScenario, core::Error>;
};

[[nodiscard]] auto expand_scenario_defaults(UnitNormalizedScenario scenario)
    -> std::expected<DefaultExpandedScenario, core::Error>;

} // namespace epycbench::scenario
