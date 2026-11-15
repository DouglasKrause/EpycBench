#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/scenario/authored_scenario.hpp"

#include <expected>
#include <string_view>

namespace epycbench::scenario {

enum class ScenarioFormat { yaml, json };

struct ScenarioSource {
    std::string_view logical_name;
    std::string_view contents;
    ScenarioFormat format;
};

[[nodiscard]] auto parse_authored_scenario(const ScenarioSource& source)
    -> std::expected<AuthoredScenario, core::Error>;

} // namespace epycbench::scenario
