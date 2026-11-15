#pragma once
#include "core/governed_repository_root_authority_access.hpp"
#include "epycbench/core/digest.hpp"
#include "monitoring/host_q1_git_fixed_results.hpp"

#include <array>
#include <expected>
#include <memory>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1PublicInputAuthorityTestAccess;
struct MonitoringQ1PublicInputIdentity final {
    std::uint64_t device{}, inode{}, size{};
    std::uint32_t mode{}, owner{}, links{};
    auto operator==(const MonitoringQ1PublicInputIdentity&) const -> bool = default;
};
class MonitoringQ1PublicInputAuthority final {
  public:
    MonitoringQ1PublicInputAuthority(const MonitoringQ1PublicInputAuthority&) = delete;
    MonitoringQ1PublicInputAuthority(MonitoringQ1PublicInputAuthority&&) noexcept;
    auto operator=(const MonitoringQ1PublicInputAuthority&)
        -> MonitoringQ1PublicInputAuthority& = delete;
    auto operator=(MonitoringQ1PublicInputAuthority&&)
        -> MonitoringQ1PublicInputAuthority& = delete;
    ~MonitoringQ1PublicInputAuthority() noexcept;
    [[nodiscard]] static auto retain(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                                     const MonitoringQ1GitFixedResults&)
        -> std::expected<MonitoringQ1PublicInputAuthority, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto recheck() const -> std::expected<bool, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto retire() -> std::expected<void, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto bytes(std::size_t index) const noexcept -> std::string_view {
        return bytes_[index];
    }
    [[nodiscard]] auto digest(std::size_t index) const noexcept -> std::string_view {
        return digests_[index];
    }
    [[nodiscard]] auto captured_head() const noexcept -> std::string_view { return captured_head_; }

    struct Operations final {
        void* context{};
        int (*open_at)(void*, int, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_at)(void*, int, const char*, struct stat*, int) noexcept {};
        ssize_t (*read_at)(void*, int, void*, std::size_t, off_t) noexcept {};
        int (*close_fd)(void*, int) noexcept {};
        unsigned int (*effective_user)(void*) noexcept {};
        void (*fault)(void*, unsigned int){};
    };

  private:
    MonitoringQ1PublicInputAuthority(std::array<int, 6>,
                                     std::array<MonitoringQ1PublicInputIdentity, 6>,
                                     std::array<std::string, 3>, std::array<std::string, 3>,
                                     std::string, std::shared_ptr<const void>, Operations) noexcept;
    static auto retain_with(int, const MonitoringQ1GitFixedResults&, Operations)
        -> std::expected<MonitoringQ1PublicInputAuthority, MonitoringQ1GitCommandFailure>;
    std::array<int, 6> descriptors_{};
    std::array<MonitoringQ1PublicInputIdentity, 6> identities_{};
    std::array<std::string, 3> bytes_{};
    std::array<std::string, 3> digests_{};
    std::string captured_head_;
    std::shared_ptr<const void> capture_binding_{};
    Operations operations_{};
    friend class MonitoringQ1PublicInputAuthorityTestAccess;
    friend class MonitoringQ1ValidatedPublicValues;
};
} // namespace epycbench::monitoring::qualification::detail
