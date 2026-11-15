#pragma once

#include "epycbench/core/error.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <string_view>

namespace epycbench::scenario::detail {

auto resolve_plain_scalar(std::string_view text)
    -> std::expected<nlohmann::ordered_json, core::Error>;

} // namespace epycbench::scenario::detail
