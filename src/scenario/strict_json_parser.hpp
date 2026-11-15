#pragma once

#include "epycbench/core/error.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <string_view>

namespace epycbench::scenario::detail {

auto parse_strict_json(std::string_view contents)
    -> std::expected<nlohmann::ordered_json, core::Error>;

auto verify_json_location_value(const nlohmann::ordered_json& strict,
                                const nlohmann::ordered_json& marked)
    -> std::expected<void, core::Error>;

} // namespace epycbench::scenario::detail
