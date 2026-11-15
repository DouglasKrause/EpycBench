#pragma once
#include "core/governed_repository_root_authority_access.hpp"
#include "monitoring/host_q1_git_command_owner.hpp"

#include <array>
#include <expected>
#include <string>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ1GitRepositoryMetadataTestOperations;
class MonitoringQ1GitRepositoryMetadataTestAccess;

struct MonitoringQ1GitMetadataIdentity final {
    std::uint64_t device{}, inode{}, size{};
    std::uint32_t mode{}, owner{}, links{};
    auto operator==(const MonitoringQ1GitMetadataIdentity&) const -> bool = default;
};

class MonitoringQ1GitRepositoryMetadata final {
  public:
    MonitoringQ1GitRepositoryMetadata(const MonitoringQ1GitRepositoryMetadata&) = delete;
    auto operator=(const MonitoringQ1GitRepositoryMetadata&)
        -> MonitoringQ1GitRepositoryMetadata& = delete;
    MonitoringQ1GitRepositoryMetadata(MonitoringQ1GitRepositoryMetadata&&) noexcept;
    auto operator=(MonitoringQ1GitRepositoryMetadata&&)
        -> MonitoringQ1GitRepositoryMetadata& = delete;
    ~MonitoringQ1GitRepositoryMetadata() noexcept;

    [[nodiscard]] static auto retain(const core::detail::OwnedGovernedRepositoryRootDescriptor&)
        -> std::expected<MonitoringQ1GitRepositoryMetadata, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto recheck() const -> std::expected<bool, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto retire() -> std::expected<void, MonitoringQ1GitCommandFailure>;

  public:
    struct Operations final {
        void* context{};
        int (*open_at)(void*, int, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_at)(void*, int, const char*, struct stat*, int) noexcept {};
        ssize_t (*read_at)(void*, int, void*, std::size_t, off_t) noexcept {};
        int (*close_fd)(void*, int) noexcept {};
        unsigned int (*effective_user)(void*) noexcept {};
        void (*fault_point)(void*, unsigned int){};
    };

  private:
    MonitoringQ1GitRepositoryMetadata(int root, std::array<int, 6>,
                                      std::array<MonitoringQ1GitMetadataIdentity, 6>,
                                      std::array<std::string, 2>, Operations) noexcept;
    [[nodiscard]] static auto retain_with(int, Operations)
        -> std::expected<MonitoringQ1GitRepositoryMetadata, MonitoringQ1GitCommandFailure>;
    int root_{};
    std::array<int, 6> descriptors_{};
    std::array<MonitoringQ1GitMetadataIdentity, 6> identities_{};
    std::array<std::string, 2> bytes_{};
    Operations operations_{};
    friend class MonitoringQ1GitRepositoryMetadataTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
