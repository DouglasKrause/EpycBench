#pragma once

#include "epycbench/core/error.hpp"

#include <nlohmann/json.hpp>

#include <expected>
#include <string_view>

namespace epycbench::topology::detail {

inline constexpr std::size_t topology_json_max_document_bytes = 64U * 1024U * 1024U;
inline constexpr std::size_t topology_json_max_depth = 32U;
inline constexpr std::size_t topology_json_max_total_objects = 262144U;
inline constexpr std::size_t topology_json_max_category_objects = 65536U;
inline constexpr std::size_t topology_json_max_array_entries = 1048576U;
inline constexpr std::size_t topology_json_max_distances = 1048576U;
inline constexpr std::size_t topology_json_max_string_bytes = 1024U * 1024U;
inline constexpr std::size_t topology_json_max_locator_bytes = 4096U;
inline constexpr std::size_t topology_json_max_evidence_entries = 4096U;

[[nodiscard]] auto parse_topology_json(std::string_view input)
    -> std::expected<nlohmann::json, core::Error>;

} // namespace epycbench::topology::detail
