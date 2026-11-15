#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace epycbench::cli {
enum class CsvError { invalid_json, limits, allocation };

// Convert an existing JSON export to path/type/value rows without interpreting
// its schema. Paths are JSON pointers; empty containers are retained as rows.
[[nodiscard]] auto json_to_csv(std::string_view json) -> std::expected<std::string, CsvError>;
} // namespace epycbench::cli
