#pragma once

#include "core/governed_repository_root_authority_access.hpp"
#include "monitoring/host_q4_complete_q3_predecessor_authenticator.hpp"

#include <array>
#include <bitset>
#include <expected>
#include <optional>
#include <set>
#include <sys/stat.h>

namespace epycbench::monitoring::qualification::detail {
class MonitoringQ4AuthoritySession;
enum class MonitoringQ4PredecessorRole : unsigned char {
    root,
    local,
    qualification,
    qid,
    q0b,
    q1,
    q2,
    q3,
    environment,
    q0b_evidence,
    q1_evidence,
    q2_evidence,
    q3_evidence,
    manifest,
    checksums,
    qid_inventory,
    q0b_inventory,
    q1_inventory,
    q2_inventory,
    q3_inventory,
    q4_directory,
    q4_evidence,
    q4_inventory
};
enum class MonitoringQ4PredecessorCleanupSource : unsigned char { inventory, descriptor };
enum class MonitoringQ4CompleteQ3PrimaryKind : unsigned char {
    operations,
    open,
    stat,
    predicate,
    inventory,
    inventory_changed,
    read,
    eof,
    growth,
    authentication,
    held_recheck,
    held_changed,
    named_recheck,
    named_changed,
    bytes_changed,
    inactive,
    contained,
    cleanup,
    projection_failed
};
struct MonitoringQ4PredecessorCleanupFact final {
    MonitoringQ4PredecessorRole role{};
    MonitoringQ4PredecessorCleanupSource source{};
    int native_error{};
};
struct MonitoringQ4CompleteQ3Retirement final {
    static constexpr std::size_t cleanup_capacity = 23;
    std::bitset<15> confirmed{}, uncertain{}, active{};
    std::bitset<2> completed_confirmed{}, completed_uncertain{}, completed_active{};
    std::array<MonitoringQ4PredecessorCleanupFact, cleanup_capacity> cleanup_facts{};
    std::size_t cleanup_count{};
    bool cleanup_overflowed{};
};
struct MonitoringQ4CompleteQ3AuthorityFailure final {
    MonitoringQ4CompleteQ3PrimaryKind primary{};
    int native_error{};
    MonitoringQ4CompleteQ3Retirement retirement{};
};
struct MonitoringQ4PredecessorCleanupRecord final {
    MonitoringQ4PredecessorRole role{};
    MonitoringQ4PredecessorCleanupSource source{};
    core::Error error{};
};
struct MonitoringQ4CompleteQ3ProjectedFailure final {
    core::Error primary;
    std::array<std::optional<MonitoringQ4PredecessorCleanupRecord>,
               MonitoringQ4CompleteQ3Retirement::cleanup_capacity> cleanup{};
    std::size_t cleanup_count{};
    MonitoringQ4CompleteQ3Retirement retirement{};
};

class MonitoringQ4CompleteQ3PredecessorAuthority final {
  public:
    static constexpr std::size_t descriptor_count = 15;
    struct Identity final {
        std::uint64_t device{}, inode{}, size{};
        std::uint32_t mode{}, owner{}, links{};
        auto operator==(const Identity&) const -> bool = default;
    };
    struct InventoryFailure final {
        MonitoringQ4CompleteQ3PrimaryKind primary{MonitoringQ4CompleteQ3PrimaryKind::inventory};
        int native_error{};
        std::optional<MonitoringQ4PredecessorCleanupFact> cleanup;
    };
    MonitoringQ4CompleteQ3PredecessorAuthority(const MonitoringQ4CompleteQ3PredecessorAuthority&) =
        delete;
    MonitoringQ4CompleteQ3PredecessorAuthority(
        MonitoringQ4CompleteQ3PredecessorAuthority&&) noexcept;
    auto operator=(MonitoringQ4CompleteQ3PredecessorAuthority&&)
        -> MonitoringQ4CompleteQ3PredecessorAuthority& = delete;
    ~MonitoringQ4CompleteQ3PredecessorAuthority() noexcept;

    static auto retain(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
                       const MonitoringQ4GovernedInputCandidate&,
                       const MonitoringQ4HistoricalGitValue&)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    auto recheck() const noexcept -> std::expected<bool, MonitoringQ4CompleteQ3AuthorityFailure>;
    auto retire() noexcept -> MonitoringQ4CompleteQ3Retirement;
    auto finalize() noexcept -> std::expected<MonitoringQ4CompleteQ3PredecessorValue,
                                              MonitoringQ4CompleteQ3AuthorityFailure>;
    auto retirement() const noexcept -> const MonitoringQ4CompleteQ3Retirement& {
        return retirement_;
    }
    static auto project_failure(const MonitoringQ4CompleteQ3AuthorityFailure&)
        -> std::expected<MonitoringQ4CompleteQ3ProjectedFailure,
                         MonitoringQ4CompleteQ3AuthorityFailure>;

  private:
    static void set_inventory_test_leaves(void (*)(), int (*)(void*) noexcept) noexcept;
    static auto production_inventory(void*, int)
        -> std::expected<std::set<std::string>, InventoryFailure>;
    struct Operations final {
        void* context{};
        int (*duplicate)(void*, int) noexcept {};
        int (*open_at)(void*, int, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_at)(void*, int, const char*, struct stat*, int) noexcept {};
        ssize_t (*read_at)(void*, int, void*, std::size_t, off_t) noexcept {};
        std::expected<std::set<std::string>, InventoryFailure> (*inventory)(void*, int){};
        int (*close_fd)(void*, int) noexcept {};
        unsigned int (*effective_user)(void*) noexcept {};
        void (*fault)(void*, unsigned int){};
    };
    struct ProjectionOperations final {
        void* context{};
        void (*fault)(void*, unsigned int){};
    };
    static auto retain_with(int, const MonitoringQ4GovernedInputCandidate&,
                            const MonitoringQ4HistoricalGitValue&, Operations,
                            const MonitoringQ4CompleteQ3PredecessorAuthenticator::ExpectedDigests*)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    static auto capture_campaign(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        const MonitoringQ4GovernedInputCandidate&, const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    static auto capture_with(int, const MonitoringQ4GovernedInputCandidate&,
        const MonitoringQ1ValidatedPublicValues&, Operations)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    static auto capture_completed(const core::detail::OwnedGovernedRepositoryRootDescriptor&,
        const MonitoringQ4GovernedInputCandidate&, const MonitoringQ1ValidatedPublicValues&)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    static auto retain_impl(int, const MonitoringQ4GovernedInputCandidate&,
        const MonitoringQ4HistoricalGitValue*, Operations,
        const MonitoringQ4CompleteQ3PredecessorAuthenticator::ExpectedDigests*,
        const MonitoringQ1ValidatedPublicValues*, bool completed = false)
        -> std::expected<MonitoringQ4CompleteQ3PredecessorAuthority,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    auto bind_campaign(const MonitoringQ4HistoricalGitValue&)
        -> std::expected<void, MonitoringQ4CompleteQ3AuthorityFailure>;
    static auto project_failure_with(const MonitoringQ4CompleteQ3AuthorityFailure&,
                                     ProjectionOperations)
        -> std::expected<MonitoringQ4CompleteQ3ProjectedFailure,
                         MonitoringQ4CompleteQ3AuthorityFailure>;
    MonitoringQ4CompleteQ3PredecessorAuthority() noexcept;
    std::array<int, descriptor_count> descriptors_{};
    std::array<Identity, descriptor_count> identities_{};
    std::array<std::string, 7> bytes_{};
    std::array<int, 2> completed_descriptors_{-1, -1};
    std::array<Identity, 2> completed_identities_{};
    std::bitset<2> completed_active_{}, completed_confirmed_{}, completed_uncertain_{};
    std::array<std::string, 2> q3_prefix_{};
    std::string q4_bytes_, producer_commit_;
    bool completed_snapshot_{};
    std::optional<MonitoringQ4CompleteQ3PredecessorValue> value_{};
    const MonitoringQ4GovernedInputCandidate* candidate_{};
    const MonitoringQ1ValidatedPublicValues* campaign_values_{};
    std::array<std::string, 4> provisional_commits_{};
    bool binding_attempted_{}, binding_completed_{};
    std::optional<MonitoringQ4HistoricalGitValue> history_{};
    std::optional<MonitoringQ4CompleteQ3PredecessorAuthenticator::ExpectedDigests> expected_{};
    Operations operations_{};
    std::bitset<descriptor_count> active_{}, confirmed_{}, uncertain_{};
    MonitoringQ4CompleteQ3Retirement retirement_{};
    friend class MonitoringQ4CompleteQ3PredecessorAuthorityTestAccess;
    friend class MonitoringQ4AuthoritySession;
    friend class MonitoringQ4HistoricalGitAuthority;
    friend class HostFrozenSourceAdmissionTestAccess;
};
} // namespace epycbench::monitoring::qualification::detail
