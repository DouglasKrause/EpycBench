#pragma once

#include "epycbench/core/governed_repository_file.hpp"

#include <expected>
#include <filesystem>
#include <string>

namespace epycbench::core::detail {

class GovernedRepositoryFileTestAccess {
  public:
    [[nodiscard]] static auto owns_root_descriptor(const GovernedRepositoryRoot& root) noexcept
        -> bool {
        return root.descriptor_ >= 0;
    }
    [[nodiscard]] static auto read_opened_regular_file(
        int descriptor, const std::filesystem::path& repository_relative_path) noexcept
        -> std::expected<GovernedRepositoryFile, Error>;
    [[nodiscard]] static auto classify_final_open_failure(
        int authoritative_errno, bool final_symlink,
        const std::filesystem::path& repository_relative_path) -> Error;
};

} // namespace epycbench::core::detail
