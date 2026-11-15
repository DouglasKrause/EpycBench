#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace epycbench::topology::detail {

[[nodiscard]] auto sha256(std::string_view data) -> std::string;
[[nodiscard]] auto sha256_file(const std::filesystem::path& path)
    -> std::expected<std::string, std::string>;

} // namespace epycbench::topology::detail
