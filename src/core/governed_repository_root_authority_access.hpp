#pragma once

#include "epycbench/core/error.hpp"
#include "epycbench/core/governed_repository_file.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <sys/stat.h>
#include <vector>

namespace epycbench::core::detail {

struct GovernedRepositoryRootIdentity final {
    std::uint64_t device;
    std::uint64_t inode;
    std::uint32_t mode;
    std::uint32_t owner;
};
struct GovernedRepositoryRootCheckResult final {
    std::optional<bool> matches;
    std::optional<Error> primary;
    std::vector<Error> cleanup;
};
struct GovernedRepositoryRootOperations final {
    void* context;
    int (*duplicate)(void*, int) noexcept;
    int (*stat_descriptor)(void*, int, struct stat*) noexcept;
    int (*open_root)(void*) noexcept;
    int (*open_directory_at)(void*, int, const char*) noexcept;
    int (*close_descriptor)(void*, int) noexcept;
    std::uint32_t (*effective_user)(void*) noexcept;
};

class OwnedGovernedRepositoryRootDescriptor final {
  public:
    OwnedGovernedRepositoryRootDescriptor(const OwnedGovernedRepositoryRootDescriptor&) = delete;
    auto operator=(const OwnedGovernedRepositoryRootDescriptor&)
        -> OwnedGovernedRepositoryRootDescriptor& = delete;
    OwnedGovernedRepositoryRootDescriptor(OwnedGovernedRepositoryRootDescriptor&&) noexcept;
    auto operator=(OwnedGovernedRepositoryRootDescriptor&&)
        -> OwnedGovernedRepositoryRootDescriptor& = delete;
    ~OwnedGovernedRepositoryRootDescriptor() noexcept;

    [[nodiscard]] auto descriptor() const noexcept -> int { return descriptor_; }
    [[nodiscard]] auto accepted_path() const noexcept -> const std::filesystem::path& {
        return accepted_path_;
    }
    [[nodiscard]] auto identity() const noexcept -> GovernedRepositoryRootIdentity {
        return identity_;
    }
    [[nodiscard]] auto still_matches_held_descriptor() const -> std::expected<bool, Error>;
    [[nodiscard]] auto still_matches_pathname() const -> GovernedRepositoryRootCheckResult;
    [[nodiscard]] auto retire() -> std::expected<void, Error>;

  private:
    OwnedGovernedRepositoryRootDescriptor(int, std::filesystem::path,
                                          GovernedRepositoryRootIdentity,
                                          GovernedRepositoryRootOperations) noexcept;
    int descriptor_{-1};
    std::filesystem::path accepted_path_;
    GovernedRepositoryRootIdentity identity_{};
    GovernedRepositoryRootOperations operations_{};
    friend class GovernedRepositoryRootAuthorityAccess;
};

struct GovernedRepositoryRootRetainResult final {
    std::optional<OwnedGovernedRepositoryRootDescriptor> value;
    std::optional<Error> primary;
    std::vector<Error> cleanup;
    operator bool() const noexcept { return value.has_value(); }
    auto operator->() noexcept -> OwnedGovernedRepositoryRootDescriptor* { return &*value; }
    auto operator*() noexcept -> OwnedGovernedRepositoryRootDescriptor& { return *value; }
    auto error() const noexcept -> const Error& { return *primary; }
};

class GovernedRepositoryRootAuthorityAccess final {
  public:
    [[nodiscard]] static auto retain(const GovernedRepositoryRoot&)
        -> GovernedRepositoryRootRetainResult;
    [[nodiscard]] static auto retain_with_operations(const GovernedRepositoryRoot&,
                                                     GovernedRepositoryRootOperations)
        -> GovernedRepositoryRootRetainResult;
};

} // namespace epycbench::core::detail
