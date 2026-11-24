#pragma once
#include <expected>
#include <filesystem>
#include <string_view>

namespace epycbench::platform_linux::kernel_modules {
[[nodiscard]] auto default_directory(std::string_view directory)
    -> std::expected<std::filesystem::path, std::string_view>;
} // namespace epycbench::platform_linux::kernel_modules
