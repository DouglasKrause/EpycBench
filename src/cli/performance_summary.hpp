#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace epycbench::cli {

// Formats existing summary statistics only. The optional complete source supplies
// user-facing names and placement labels omitted from the reduced JSON profile.
[[nodiscard]] auto render_performance_summary(std::string_view summary_json,
    std::size_t columns = 80, std::size_t rows = 24,
    std::string_view source_json = {}) -> std::string;

} // namespace epycbench::cli
