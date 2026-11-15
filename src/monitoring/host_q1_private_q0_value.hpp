#pragma once
#include "core/governed_repository_root_authority_access.hpp"
#include "monitoring/host_q1_git_command_owner.hpp"
#include "monitoring/host_q1_validated_public_values.hpp"
#include <array>
#include <expected>
#include <string>
#include <vector>
namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1PrivateQ0ValueTestAccess;
struct MonitoringQ1PrivateQ0Identity final {
    std::uint64_t device{}, inode{}, size{};
    std::uint32_t mode{}, owner{}, links{};
    auto operator==(const MonitoringQ1PrivateQ0Identity&) const -> bool = default;
};
struct MonitoringQ1PrivateQ0ListResult final {
    int result{};
    int primary_errno{};
    int cleanup_errno{};
};
class MonitoringQ1PrivateQ0Value final {
  public:
    MonitoringQ1PrivateQ0Value(const MonitoringQ1PrivateQ0Value&) = delete;
    MonitoringQ1PrivateQ0Value(MonitoringQ1PrivateQ0Value&&) noexcept;
    auto operator=(MonitoringQ1PrivateQ0Value&&) -> MonitoringQ1PrivateQ0Value& = delete;
    ~MonitoringQ1PrivateQ0Value() noexcept;
    static auto retain(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                       const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto recheck(const MonitoringQ1ValidatedPublicValues&) const
        -> std::expected<bool, MonitoringQ1GitCommandFailure>;
    [[nodiscard]] auto retire() -> std::expected<void, MonitoringQ1GitCommandFailure>;
    struct Operations final {
        void* context{};
        int (*duplicate_fd)(void*, int) noexcept {};
        int (*open_at)(void*, int, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_at)(void*, int, const char*, struct stat*, int) noexcept {};
        ssize_t (*read_at)(void*, int, void*, std::size_t, off_t) noexcept {};
        MonitoringQ1PrivateQ0ListResult (*list_dir)(void*, int, std::vector<std::string>*){};
        int (*close_fd)(void*, int) noexcept {};
        unsigned int (*effective_user)(void*) noexcept {};
        void (*fault)(void*, unsigned int){};
        void (*before_recheck_projection)(void*){};
        void (*before_retire_projection)(void*){};
    };

  private:
    MonitoringQ1PrivateQ0Value(int, std::array<int, 8>,
                               std::array<MonitoringQ1PrivateQ0Identity, 8>,
                               std::array<std::string, 4>, Operations, core::Error, core::Error,
                               core::Error, core::Error) noexcept;
    static auto retain_with(int, const MonitoringQ1ValidatedPublicValues&, Operations)
        -> std::expected<MonitoringQ1PrivateQ0Value, MonitoringQ1GitCommandFailure>;
    static auto validate(const std::array<std::string, 4>&,
                         const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<void, MonitoringQ1GitCommandFailure>;
    // Validate the original two-leaf projection from a retained successor snapshot.
    // No descriptors are reopened and no authority is derived from caller digests.
    static auto validate_successor_subset(std::string_view, std::string_view,
        const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<void, MonitoringQ1GitCommandFailure>;
    static auto validate_document_for_test(std::size_t, const std::array<std::string, 4>&,
                                           const MonitoringQ1ValidatedPublicValues&) -> bool;
    std::array<int, 8> descriptors_{};
    int root_descriptor_{-1};
    std::array<MonitoringQ1PrivateQ0Identity, 8> identities_{};
    std::array<std::string, 4> bytes_;
    Operations operations_{};
    mutable core::Error recheck_standard_, recheck_unknown_;
    core::Error retire_standard_, retire_unknown_;
    friend class MonitoringQ1PrivateQ0ValueTestAccess;
    friend class MonitoringQ3CompleteQ2PredecessorAuthenticator;
    friend class MonitoringQ4CompleteQ3PredecessorAuthenticator;
};
} // namespace epycbench::monitoring::qualification::detail
