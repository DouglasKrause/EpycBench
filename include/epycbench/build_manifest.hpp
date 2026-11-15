#pragma once

#include <nlohmann/json.hpp>

namespace epycbench {

[[nodiscard]] auto build_manifest_json() -> nlohmann::json;

} // namespace epycbench
