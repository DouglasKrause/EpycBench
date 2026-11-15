#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace epycbench::platform_linux::presentation_files {
enum class Stage { argument, directory, identity, open, write, close, status };
struct Failure {
    Stage stage{};
    int error_number{};
};
struct FileStatus {
    bool exists{};
    std::int64_t modified_seconds{};
};
struct TerminalDimensions {
    std::size_t columns{80}, rows{24};
};
[[nodiscard]] auto ensure_private_subdirectory(const std::filesystem::path& parent,
    std::string_view name) -> std::expected<std::string,Failure>;
[[nodiscard]] auto save_numbered(const std::filesystem::path& directory,
    std::string_view filename,std::string_view bytes) -> std::expected<std::string,Failure>;
[[nodiscard]] auto file_status(const std::filesystem::path&) -> std::expected<FileStatus,Failure>;
[[nodiscard]] auto stdout_is_terminal() noexcept -> bool;
[[nodiscard]] auto terminal_dimensions() noexcept -> TerminalDimensions;
} // namespace epycbench::platform_linux::presentation_files
