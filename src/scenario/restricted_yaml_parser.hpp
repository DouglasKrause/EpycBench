#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/scenario/authored_scenario.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <map>
#include <string>
#include <string_view>

namespace epycbench::scenario::detail {

struct ParsedDocument {
    nlohmann::ordered_json value;
    std::map<std::string, NodeLocations> locations;
};

auto parse_restricted_yaml(std::string_view contents, bool json_location_mode = false)
    -> std::expected<ParsedDocument, core::Error>;

} // namespace epycbench::scenario::detail
