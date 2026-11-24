#pragma once

#include <cstddef>
#include <string_view>

namespace epycbench::cli::run_output_directory {

inline constexpr std::size_t path_limit = 4095U, component_limit = 255U, component_count = 256U;

[[nodiscard]] inline auto valid(std::string_view value) noexcept -> bool {
    if (value.size() < 2U || value.size() > path_limit || value.front() != '/' ||
        value.back() == '/' || value.find('\0') != std::string_view::npos) return false;
    std::size_t offset = 1U, count{};
    while (offset < value.size()) {
        const auto end = value.find('/', offset);
        const auto part = value.substr(offset, end == std::string_view::npos ? value.size() - offset : end - offset);
        if (part.empty() || part.size() > component_limit || part == "." || part == ".." || ++count > component_count)
            return false;
        if (end == std::string_view::npos) break;
        offset = end + 1U;
    }
    return true;
}

} // namespace epycbench::cli::run_output_directory
