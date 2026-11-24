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

} // namespace epycbench::scenario
