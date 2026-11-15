#include "epycbench/scenario/unit_normalization.hpp"
#include "core/json_ownership.hpp"
#include "scenario/json_cleanup.hpp"

#include "scenario/scenario_v1_units.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::scenario {
namespace detail {

auto make_units_internal_error(std::string classification) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};

    context["classification"] = std::move(classification);
    return core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"scenario.units.internal_failure"},
        .message = "Scenario unit normalization failed internally",
        .operation = "normalize_scenario_units",
        .os_error = std::nullopt,
        .context = std::move(context),
    };
}

} // namespace detail

UnitNormalizedScenario::UnitNormalizedScenario(StructurallyValidScenario scenario,
                                               nlohmann::ordered_json normalized_value)
    : scenario_(std::move(scenario)), normalized_value_(std::move(normalized_value)) {}

UnitNormalizedScenario::UnitNormalizedScenario(const UnitNormalizedScenario& source)
    : scenario_(source.scenario_), normalized_value_(detail::clone_json(source.normalized_value_)) {
}
UnitNormalizedScenario::~UnitNormalizedScenario() { detail::retire_json(normalized_value_); }
auto UnitNormalizedScenario::operator=(UnitNormalizedScenario&& source) noexcept
    -> UnitNormalizedScenario& {
    if (this != &source) {
        detail::retire_json(normalized_value_);
        scenario_ = std::move(source.scenario_);
        normalized_value_ = std::move(source.normalized_value_);
    }
    return *this;
}
auto UnitNormalizedScenario::operator=(const UnitNormalizedScenario& source)
    -> UnitNormalizedScenario& {
    UnitNormalizedScenario copy{source};
    return *this = std::move(copy);
}

auto normalize_scenario_units(StructurallyValidScenario scenario)
    -> std::expected<UnitNormalizationResult, core::Error> {
    try {
        auto normalized = detail::clone_json(scenario.value());
        detail::JsonCleanup cleanup{normalized};
        auto diagnostics = detail::normalize_v1_units(scenario, normalized);
        if (!diagnostics)
            return std::unexpected{std::move(diagnostics.error())};
        if (!diagnostics->empty()) {
            return UnitNormalizationResult{.scenario = std::nullopt,
                                           .diagnostics = std::move(*diagnostics)};
        }
        return UnitNormalizationResult{
            .scenario = UnitNormalizedScenario{std::move(scenario), std::move(normalized)},
            .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::make_units_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::make_units_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
