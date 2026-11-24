#pragma once

#include "epycbench/platform_linux/file_descriptor.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epycbench::platform_linux::experiment_operations {

class Directory;

struct Cancellation {
    void* context{};
    bool (*requested)(void*) noexcept{};
};

struct RunExit {
    std::optional<int> exit_code;
    std::optional<int> signal;
    bool cancelled{};
};

[[nodiscard]] auto run(const Directory&, const std::filesystem::path& scenario,
                       std::string_view expected_plan_digest,
                       const std::optional<std::filesystem::path>& module_directory,
                       const std::filesystem::path& runtime_directory, Cancellation,
                       const std::optional<std::filesystem::path>& supplied_workers = {})
    -> std::expected<RunExit, core::Error>;

// Operations on one owner are sequential and relative to the held directory.
// Closing the owner releases admission, subject to any still-active Run lease.
class Directory {
  public:
    [[nodiscard]] static auto acquire(const std::filesystem::path&)
        -> std::expected<Directory, core::Error>;

    Directory(Directory&&) noexcept = default;
    auto operator=(Directory&&) noexcept -> Directory& = default;
    Directory(const Directory&) = delete;
    auto operator=(const Directory&) -> Directory& = delete;

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path& { return path_; }
    [[nodiscard]] auto write_new(std::string_view name, std::string_view bytes,
                                 std::size_t max_bytes) const
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto read(std::string_view name, std::size_t max_bytes) const
        -> std::expected<std::string, core::Error>;
    [[nodiscard]] auto sync_file(std::string_view name) const
        -> std::expected<void, core::Error>;
    [[nodiscard]] auto list(std::size_t max_entries) const
        -> std::expected<std::vector<std::string>, core::Error>;
    [[nodiscard]] auto create_subdirectory(std::string_view name) const
        -> std::expected<std::filesystem::path, core::Error>;

  private:
    Directory(std::filesystem::path, FileDescriptor) noexcept;

    std::filesystem::path path_;
    FileDescriptor descriptor_;

    friend auto run(const Directory&, const std::filesystem::path&,
                    std::string_view, const std::optional<std::filesystem::path>&,
                    const std::filesystem::path&, Cancellation, const std::optional<std::filesystem::path>&)
        -> std::expected<RunExit, core::Error>;
};

} // namespace epycbench::platform_linux::experiment_operations
