#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace epycbench::monitoring::qualification::detail {

[[nodiscard]] auto privacy_prohibited(std::string_view value) -> bool;
[[nodiscard]] auto privacy_safe(const nlohmann::json& value) -> bool;

} // namespace epycbench::monitoring::qualification::detail
