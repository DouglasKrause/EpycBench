#pragma once

#include "epycbench/core/error.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace epycbench::core {

namespace detail {
class GovernedRepositoryFileTestAccess;
class GovernedRepositoryRootAuthorityAccess;
class GovernedRepositorySourceAccess;
} // namespace detail

class GovernedRepositoryFile {
  public:
    [[nodiscard]] auto repository_relative_path() const noexcept -> const std::filesystem::path& {
        return repository_relative_path_;
    }
    [[nodiscard]] auto bytes() const noexcept -> const std::string& { return bytes_; }

  private:
    friend class GovernedRepositoryRoot;
    friend class detail::GovernedRepositoryFileTestAccess;
    friend class detail::GovernedRepositorySourceAccess;
    GovernedRepositoryFile(std::filesystem::path repository_relative_path, std::string bytes) noexcept
        : repository_relative_path_{std::move(repository_relative_path)}, bytes_{std::move(bytes)} {
    }

    std::filesystem::path repository_relative_path_;
    std::string bytes_;
};

class GovernedRepositoryRoot {
  public:
    [[nodiscard]] static auto create(const std::filesystem::path& root) noexcept
        -> std::expected<GovernedRepositoryRoot, Error>;

    GovernedRepositoryRoot(const GovernedRepositoryRoot&) = delete;
    auto operator=(const GovernedRepositoryRoot&) -> GovernedRepositoryRoot& = delete;
    GovernedRepositoryRoot(GovernedRepositoryRoot&& other) noexcept;
    auto operator=(GovernedRepositoryRoot&&) -> GovernedRepositoryRoot& = delete;
    ~GovernedRepositoryRoot();

    [[nodiscard]] auto
    read_regular_file(const std::filesystem::path& repository_relative_path) const noexcept
        -> std::expected<GovernedRepositoryFile, Error>;

  private:
    friend class detail::GovernedRepositoryFileTestAccess;
    friend class detail::GovernedRepositoryRootAuthorityAccess;
    GovernedRepositoryRoot(int descriptor, std::filesystem::path accepted_root) noexcept
        : descriptor_{descriptor}, accepted_root_{std::move(accepted_root)} {}
    [[nodiscard]] static auto
    read_opened_regular_file(int descriptor,
                             const std::filesystem::path& repository_relative_path) noexcept
        -> std::expected<GovernedRepositoryFile, Error>;

    int descriptor_{-1};
    std::filesystem::path accepted_root_;
};

} // namespace epycbench::core
