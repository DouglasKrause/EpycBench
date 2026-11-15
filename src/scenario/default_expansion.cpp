#include "epycbench/scenario/default_expansion.hpp"
#include "scenario/json_cleanup.hpp"

#include "scenario/scenario_v1_defaults.hpp"

#include <exception>
#include <optional>
#include <utility>

namespace epycbench::scenario {

DefaultExpandedScenario::DefaultExpandedScenario(UnitNormalizedScenario scenario,
                                                 nlohmann::ordered_json expanded_value,
                                                 std::map<std::string, ValueOrigin> origins)
    : scenario_(std::move(scenario)), origins_(std::move(origins)),
      expanded_value_(std::move(expanded_value)) {}

DefaultExpandedScenario::DefaultExpandedScenario(const DefaultExpandedScenario& source)
    : scenario_(source.scenario_), origins_(source.origins_),
      expanded_value_(detail::clone_json(source.expanded_value_)) {}
DefaultExpandedScenario::~DefaultExpandedScenario() { detail::retire_json(expanded_value_); }
auto DefaultExpandedScenario::operator=(DefaultExpandedScenario&& source) noexcept
    -> DefaultExpandedScenario& {
    if (this != &source) {
        detail::retire_json(expanded_value_);
        scenario_ = std::move(source.scenario_);
        origins_ = std::move(source.origins_);
        expanded_value_ = std::move(source.expanded_value_);
    }
    return *this;
}
auto DefaultExpandedScenario::operator=(const DefaultExpandedScenario& source)
    -> DefaultExpandedScenario& {
    DefaultExpandedScenario copy{source};
    return *this = std::move(copy);
}

auto expand_scenario_defaults(UnitNormalizedScenario scenario)
    -> std::expected<DefaultExpandedScenario, core::Error> {
    try {
        auto effective = detail::clone_json(scenario.value());
        detail::JsonCleanup cleanup{effective};
        std::map<std::string, ValueOrigin> origins;
        auto expanded = detail::expand_v1_defaults(effective, origins);
        if (!expanded)
            return std::unexpected{std::move(expanded.error())};
        return DefaultExpandedScenario{std::move(scenario), std::move(effective),
                                       std::move(origins)};
    } catch (const std::exception&) {
        return std::unexpected{
            detail::make_defaults_internal_error("contained_exception", "public_boundary")};
    } catch (...) {
        return std::unexpected{
            detail::make_defaults_internal_error("contained_unknown_failure", "public_boundary")};
    }
}

} // namespace epycbench::scenario
