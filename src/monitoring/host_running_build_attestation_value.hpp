#pragma once
#include "epycbench/core/error.hpp"
#include <expected>
#include <nlohmann/json.hpp>
#include <string_view>
namespace epycbench::monitoring::qualification::detail {
[[nodiscard]] auto attest_monitoring_running_build_value(const nlohmann::json&,std::string_view)
 ->std::expected<nlohmann::ordered_json,core::Error>;
}
