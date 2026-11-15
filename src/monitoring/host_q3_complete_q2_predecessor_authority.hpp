#pragma once

#include "core/governed_repository_root_authority_access.hpp"
#include "monitoring/host_q3_complete_q2_predecessor_authenticator.hpp"

#include <array>
#include <expected>
#include <optional>
#include <set>
#include <sys/stat.h>

namespace epycbench::monitoring::qualification::detail {

struct MonitoringQ3CompleteQ2PredecessorAuthorityFailure final {
    core::Error primary;
    std::array<std::optional<core::Error>, 48> cleanup{};
    std::size_t cleanup_count{};
};

class MonitoringQ3CompleteQ2PredecessorBinding final {
  public:
    MonitoringQ3CompleteQ2PredecessorBinding(const MonitoringQ3CompleteQ2PredecessorBinding&) = delete;
    MonitoringQ3CompleteQ2PredecessorBinding(MonitoringQ3CompleteQ2PredecessorBinding&&) = default;

  private:
    MonitoringQ3CompleteQ2PredecessorBinding(std::string_view qualification_id,
        std::string_view current_commit, std::string_view profile, std::string_view q1_plan,
        std::string_view q2_plan, std::string_view q3_plan,
        MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history,
        const MonitoringQ1ValidatedPublicValues* campaign_values = nullptr) noexcept;
    std::string_view qualification_id_, current_commit_, profile_, q1_plan_, q2_plan_, q3_plan_;
    MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history_;
    const MonitoringQ1ValidatedPublicValues* campaign_values_{};
    friend class MonitoringQ3CompleteQ2PredecessorAuthority;
    friend class MonitoringQ3CompleteQ2PredecessorAuthorityTestAccess;
    friend class MonitoringQ3AuthoritySession;
};

class MonitoringQ3CompleteQ2PredecessorAuthority final {
  public:
    struct SnapshotIdentity final {
        std::uint64_t device{}, inode{}, size{};
        std::uint32_t mode{}, owner{}, links{};
        auto operator==(const SnapshotIdentity&) const -> bool = default;
    };
    struct InventoryFailure final { core::Error primary; std::optional<core::Error> cleanup; };
    MonitoringQ3CompleteQ2PredecessorAuthority(
        const MonitoringQ3CompleteQ2PredecessorAuthority&) = delete;
    MonitoringQ3CompleteQ2PredecessorAuthority(
        MonitoringQ3CompleteQ2PredecessorAuthority&&) noexcept;
    auto operator=(MonitoringQ3CompleteQ2PredecessorAuthority&&)
        -> MonitoringQ3CompleteQ2PredecessorAuthority& = delete;
    ~MonitoringQ3CompleteQ2PredecessorAuthority() noexcept;

    [[nodiscard]] static auto retain(
        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        const MonitoringQ3CompleteQ2PredecessorBinding&)
        -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                         MonitoringQ3CompleteQ2PredecessorAuthorityFailure>;
    [[nodiscard]] auto recheck() const
        -> std::expected<bool, MonitoringQ3CompleteQ2PredecessorAuthorityFailure>;
    [[nodiscard]] auto retire()
        -> std::expected<void, MonitoringQ3CompleteQ2PredecessorAuthorityFailure>;
    [[nodiscard]] auto value() const noexcept
        -> const MonitoringQ3CompleteQ2PredecessorValue& { return *value_; }

  private:
    struct Operations final {
        void* context{};
        int (*duplicate)(void*, int) noexcept{};
        int (*open_at)(void*, int, const char*, int) noexcept{};
        int (*stat_fd)(void*, int, struct stat*) noexcept{};
        int (*stat_at)(void*, int, const char*, struct stat*, int) noexcept{};
        ssize_t (*read_at)(void*, int, void*, std::size_t, off_t) noexcept{};
        std::expected<std::set<std::string>, InventoryFailure> (*inventory)(void*, int){};
        int (*close_fd)(void*, int) noexcept{};
        unsigned int (*effective_user)(void*) noexcept{};
        void (*fault)(void*, unsigned int){};
    };
    [[nodiscard]] static auto retain_with(int, const MonitoringQ3CompleteQ2PredecessorBinding&,
        Operations, const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests*)
        -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                         MonitoringQ3CompleteQ2PredecessorAuthorityFailure>;
    [[nodiscard]] static auto retain_with_native_expected(int,
        const MonitoringQ3CompleteQ2PredecessorBinding&,
        const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests&)
        -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                         MonitoringQ3CompleteQ2PredecessorAuthorityFailure>;
    [[nodiscard]] static auto retain_with_native_fault(int,
        const MonitoringQ3CompleteQ2PredecessorBinding&,
        const MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests&, void*,
        void (*)(void*, unsigned int))
        -> std::expected<MonitoringQ3CompleteQ2PredecessorAuthority,
                         MonitoringQ3CompleteQ2PredecessorAuthorityFailure>;
    MonitoringQ3CompleteQ2PredecessorAuthority() noexcept;
    std::array<int, 13> descriptors_{};
    std::array<SnapshotIdentity, 13> identities_{};
    std::array<std::string, 6> bytes_{};
    std::string qualification_id_, current_commit_, profile_, q1_plan_, q2_plan_, q3_plan_;
    std::optional<MonitoringQ3CompleteQ2PredecessorValue> value_{};
    MonitoringQ3CompleteQ2PredecessorAuthenticator::Operations history_{};
    std::optional<MonitoringQ3CompleteQ2PredecessorAuthenticator::ExpectedDigests> expected_{};
    const MonitoringQ1ValidatedPublicValues* campaign_values_{};
    Operations operations_{};
    bool active_{};
    friend class MonitoringQ3CompleteQ2PredecessorAuthorityTestAccess;
};

} // namespace epycbench::monitoring::qualification::detail
