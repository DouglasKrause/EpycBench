#pragma once
#include "epycbench/core/digest.hpp"
#include "epycbench/core/error.hpp"
#include "core/governed_repository_root_authority_access.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <array>
#include <sys/stat.h>
#include <vector>
#include <set>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ1ValidatedPublicValues;

struct MonitoringQ2HistoricalFailure final {
    core::Error primary;
    std::array<std::optional<core::Error>,16> cleanup{};
    std::size_t cleanup_count{};
};

struct MonitoringQ2CompleteQ1PredecessorFailure final {
    // A historical authority can contribute its complete fixed ledger, followed by
    // every descriptor owned here and the three inventory-close secondaries.
    static constexpr std::size_t historical_cleanup_failures = 16;
    static constexpr std::size_t owned_retirement_failures = 12;
    static constexpr std::size_t maximum_cleanup_failures =
        historical_cleanup_failures + owned_retirement_failures;
    core::Error primary;
    std::array<std::optional<core::Error>, maximum_cleanup_failures> cleanup{};
    std::size_t cleanup_count{};
    MonitoringQ2CompleteQ1PredecessorFailure(core::Error value) : primary{std::move(value)} {}
    void append(core::Error value) noexcept {
        if (cleanup_count == cleanup.size()) std::terminate();
        cleanup[cleanup_count++].emplace(std::move(value));
    }
    void append(MonitoringQ2CompleteQ1PredecessorFailure value) noexcept {
        append(std::move(value.primary));
        for (std::size_t index{}; index < value.cleanup_count; ++index)
            append(std::move(*value.cleanup[index]));
    }
};

struct MonitoringQ2CompleteQ1PredecessorInput final {
    std::string qualification_id;
    std::string current_commit;
    std::string profile_bytes;
    std::string q1_plan_bytes;
    std::string host_environment_bytes;
    std::string q0b_evidence_bytes;
    std::string q1_evidence_bytes;
    std::string manifest_bytes;
    std::string checksums_bytes;
};

struct MonitoringQ2CompleteQ1PredecessorOperations final {
    void* context{};
    std::expected<void, MonitoringQ2HistoricalFailure> (*authenticate_historical)(
        void*, std::string_view historical_commit, std::string_view current_commit,
        std::string_view q0_commit, std::string_view profile_bytes, std::string_view q1_plan_bytes){};
    std::expected<void, core::Error> (*retire)(void*) noexcept {};
};

class MonitoringQ2CompleteQ1PredecessorBinding final {
  public:
    MonitoringQ2CompleteQ1PredecessorBinding(const MonitoringQ2CompleteQ1PredecessorBinding&)=delete;
    MonitoringQ2CompleteQ1PredecessorBinding(MonitoringQ2CompleteQ1PredecessorBinding&&) noexcept=default;
  private:
    MonitoringQ2CompleteQ1PredecessorBinding(const MonitoringQ1ValidatedPublicValues& values,
      std::string_view current,std::string_view profile,std::string_view plan,
      MonitoringQ2CompleteQ1PredecessorOperations ops) noexcept;
    std::string_view qualification_id,current_commit,profile_bytes,q1_plan_bytes;
    MonitoringQ2CompleteQ1PredecessorOperations historical;
    const MonitoringQ1ValidatedPublicValues* validated;
    friend class MonitoringQ2CompleteQ1PredecessorAuthority;
    friend class MonitoringQ2CompleteQ1PredecessorTestAccess;
    friend class MonitoringQ2AuthoritySession;
};

class MonitoringQ2CompleteQ1PredecessorAuthority final {
  public:
    struct SnapshotIdentity final {
        std::uint64_t device{}, inode{}, size{};
        std::uint32_t mode{}, owner{}, links{};
        auto operator==(const SnapshotIdentity&) const -> bool = default;
    };
    struct InventoryFailure final {
        core::Error primary;
        std::optional<core::Error> cleanup;
    };
    MonitoringQ2CompleteQ1PredecessorAuthority(
        const MonitoringQ2CompleteQ1PredecessorAuthority&) = delete;
    auto operator=(const MonitoringQ2CompleteQ1PredecessorAuthority&)
        -> MonitoringQ2CompleteQ1PredecessorAuthority& = delete;
    MonitoringQ2CompleteQ1PredecessorAuthority(
        MonitoringQ2CompleteQ1PredecessorAuthority&&) noexcept;
    auto operator=(MonitoringQ2CompleteQ1PredecessorAuthority&&)
        -> MonitoringQ2CompleteQ1PredecessorAuthority& = delete;
    ~MonitoringQ2CompleteQ1PredecessorAuthority() noexcept;

    [[nodiscard]] static auto retain(
        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        const MonitoringQ2CompleteQ1PredecessorBinding&)
        -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                         MonitoringQ2CompleteQ1PredecessorFailure>;
    [[nodiscard]] auto recheck() const
        -> std::expected<bool, MonitoringQ2CompleteQ1PredecessorFailure>;
    [[nodiscard]] auto retire() -> std::expected<void, MonitoringQ2CompleteQ1PredecessorFailure>;

    [[nodiscard]] auto historical_q1_commit() const noexcept -> std::string_view { return commit_; }
    [[nodiscard]] auto predecessor_manifest_sha256() const noexcept -> const core::Sha256Digest& {
        return manifest_;
    }
    [[nodiscard]] auto predecessor_checksums_sha256() const noexcept -> const core::Sha256Digest& {
        return checksums_;
    }
    [[nodiscard]] auto q1_evidence_sha256() const noexcept -> const core::Sha256Digest& { return q1_; }
    [[nodiscard]] auto host_environment_digest() const noexcept -> std::string_view { return environment_; }
    [[nodiscard]] auto qualification_plan_digest() const noexcept -> std::string_view { return qualification_plan_; }
    [[nodiscard]] auto q0_profile_repository_commit() const noexcept -> std::string_view { return q0_commit_; }

  private:
    struct SnapshotOperations final {
        void* context{};
        int (*duplicate)(void*,int) noexcept{};
        int (*open_at)(void*,int,const char*,int) noexcept{};
        int (*stat_fd)(void*,int,struct stat*) noexcept{};
        int (*stat_at)(void*,int,const char*,struct stat*,int) noexcept{};
        ssize_t (*read_at)(void*,int,void*,std::size_t,off_t) noexcept{};
        std::expected<std::set<std::string>,InventoryFailure> (*inventory)(void*,int){};
        int (*close_fd)(void*,int) noexcept{};
        unsigned int (*effective_user)(void*) noexcept{};
        void (*fault)(void*,unsigned int){};
    };
    [[nodiscard]] static auto authenticate(MonitoringQ2CompleteQ1PredecessorInput,
                                           MonitoringQ2CompleteQ1PredecessorOperations,
                                           std::array<std::optional<core::Error>,
                                               MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures>&)
        -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                         MonitoringQ2CompleteQ1PredecessorFailure>;
    [[nodiscard]] static auto retain_with(int,const MonitoringQ2CompleteQ1PredecessorBinding&,
                                          SnapshotOperations)
        -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                         MonitoringQ2CompleteQ1PredecessorFailure>;
    [[nodiscard]] static auto retain_with_fault(
        const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        const MonitoringQ2CompleteQ1PredecessorBinding&,void*,void (*)(void*,unsigned int))
        -> std::expected<MonitoringQ2CompleteQ1PredecessorAuthority,
                         MonitoringQ2CompleteQ1PredecessorFailure>;
    MonitoringQ2CompleteQ1PredecessorAuthority(std::string, std::string, std::string,
                                               std::string, std::string, std::string, std::string, std::string,
                                               core::Sha256Digest, core::Sha256Digest, core::Sha256Digest,
                                               MonitoringQ2CompleteQ1PredecessorOperations,
                                               std::array<std::optional<core::Error>,
                                                   MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures>) noexcept;
    std::array<int, 11> descriptors_{};
    std::array<SnapshotIdentity, 11> identities_{};
    std::array<std::string, 5> retained_bytes_{};
    std::string commit_;
    std::string environment_, qualification_plan_, q0_commit_;
    std::string qualification_id_, current_commit_, profile_bytes_, q1_plan_bytes_;
    core::Sha256Digest manifest_, checksums_, q1_;
    MonitoringQ2CompleteQ1PredecessorOperations operations_{};
    SnapshotOperations snapshot_operations_{};
    std::array<std::optional<core::Error>,
        MonitoringQ2CompleteQ1PredecessorFailure::maximum_cleanup_failures> cleanup_stock_{};
    std::optional<core::Error> retirement_primary_{};
    bool active_{true};
    friend class MonitoringQ2CompleteQ1PredecessorTestAccess;
    friend class MonitoringQ3CompleteQ2PredecessorAuthenticator;
};
} // namespace epycbench::monitoring::qualification::detail
