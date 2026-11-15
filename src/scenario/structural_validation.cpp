#include "epycbench/scenario/structural_validation.hpp"
#include "core/json_ownership.hpp"

#include "scenario/scenario_v1_structure.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace epycbench::scenario {
namespace detail {

auto make_structure_internal_error(std::string classification) -> core::Error {
    auto context = nlohmann::json::object();
    core::detail::JsonCleanup context_cleanup{context};

    context["classification"] = std::move(classification);
    return core::Error{
        .category = core::ErrorCategory::scenario,
        .code = core::StableErrorCode{"scenario.structure.internal_failure"},
        .message = "Scenario structural validation failed internally",
        .operation = "validate_scenario_structure",
        .os_error = std::nullopt,
        .context = std::move(context),
    };
}

} // namespace detail

StructurallyValidScenario::StructurallyValidScenario(AuthoredScenario authored)
    : authored_(std::move(authored)) {}

auto validate_scenario_structure(AuthoredScenario authored)
    -> std::expected<StructuralValidationResult, core::Error> {
    try {
        auto diagnostics = detail::validate_v1_structure(authored);
        if (!diagnostics.empty()) {
            return StructuralValidationResult{.scenario = std::nullopt,
                                              .diagnostics = std::move(diagnostics)};
        }
        return StructuralValidationResult{
            .scenario = StructurallyValidScenario{std::move(authored)}, .diagnostics = {}};
    } catch (const std::exception&) {
        return std::unexpected{detail::make_structure_internal_error("contained_exception")};
    } catch (...) {
        return std::unexpected{detail::make_structure_internal_error("contained_unknown_failure")};
    }
}

} // namespace epycbench::scenario
