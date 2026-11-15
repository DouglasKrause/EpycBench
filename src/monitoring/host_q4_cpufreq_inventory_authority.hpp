#pragma once

#include "monitoring/host_q4_source_ready.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace epycbench::monitoring::qualification::detail {

enum class MonitoringQ4CpuFreqInventoryFailureKind : unsigned char {
    capacity,
    allocation,
    open_root,
    enumerate,
    root_name_limit,
    root_entry_limit,
    policy_name,
    policy_limit,
    open_policy,
    open_attribute,
    held_stat,
    named_stat,
    identity,
    read,
    read_contract,
    byte_limit,
    close,
    contained,
    policy_callback,
    inactive
};

enum class MonitoringQ4CpuFreqRole : unsigned char { none, attribute, policy, root };

struct MonitoringQ4CpuFreqCleanup final {
    MonitoringQ4CpuFreqRole role{};
    std::uint64_t policy_ordinal{};
    std::uint64_t attribute_ordinal{};
    int native_error{};
    bool uncertain{};
};

struct MonitoringQ4CpuFreqInventoryFailure final {
    MonitoringQ4CpuFreqInventoryFailureKind primary{};
    int native_error{};
    bool primary_uncertain{};
    MonitoringQ4CpuFreqRole primary_role{MonitoringQ4CpuFreqRole::none};
    std::uint64_t primary_policy_ordinal{};
    std::uint64_t primary_attribute_ordinal{};
    std::vector<MonitoringQ4CpuFreqCleanup> cleanup;
    bool cleanup_overflowed{};
    MonitoringQ4CapacityReason reason{MonitoringQ4CapacityReason::none};
};

enum class MonitoringQ4CpuFreqAttribute : unsigned char {
    related_cpus,
    scaling_driver,
    scaling_governor,
    scaling_min_freq,
    scaling_max_freq,
    cpuinfo_cur_freq,
    scaling_cur_freq
};

struct MonitoringQ4CpuFreqIdentity final {
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t owner{};
    std::uint64_t mode{};
    std::uint64_t links{};
};

struct MonitoringQ4CpuFreqAttributeBytes final {
    bool present{};
    bool hardware_unreadable{};
    std::uint64_t offset{};
    std::uint64_t size{};
    MonitoringQ4CpuFreqIdentity identity{};
};

struct MonitoringQ4CpuFreqPolicyInventory final {
    std::uint32_t policy_number{};
    MonitoringQ4CpuFreqIdentity identity{};
    std::array<MonitoringQ4CpuFreqAttributeBytes, 7> attributes{};
};

class MonitoringQ4CpuFreqInventory final {
  public:
    MonitoringQ4CpuFreqInventory(const MonitoringQ4CpuFreqInventory&) = delete;
    MonitoringQ4CpuFreqInventory(MonitoringQ4CpuFreqInventory&&) noexcept = default;
    auto operator=(const MonitoringQ4CpuFreqInventory&) -> MonitoringQ4CpuFreqInventory& = delete;
    auto operator=(MonitoringQ4CpuFreqInventory&&) -> MonitoringQ4CpuFreqInventory& = delete;
    [[nodiscard]] auto policies() const noexcept
        -> std::span<const MonitoringQ4CpuFreqPolicyInventory> {
        return policies_;
    }
    [[nodiscard]] auto bytes(const MonitoringQ4CpuFreqAttributeBytes& value) const noexcept
        -> std::string_view {
        if (!value.present || value.hardware_unreadable || value.offset > contents_.size() ||
            value.size > contents_.size() - value.offset)
            return {};
        return {contents_.data() + value.offset, static_cast<std::size_t>(value.size)};
    }

  private:
    MonitoringQ4CpuFreqInventory(MonitoringQ4CpuFreqIdentity root,
                                 std::vector<MonitoringQ4CpuFreqPolicyInventory> policies,
                                 std::vector<char> contents,
                                 std::shared_ptr<const void> binding) noexcept
        : root_identity_{root}, policies_{std::move(policies)}, contents_{std::move(contents)},
          binding_{std::move(binding)} {}
    MonitoringQ4CpuFreqIdentity root_identity_{};
    std::vector<MonitoringQ4CpuFreqPolicyInventory> policies_;
    std::vector<char> contents_;
    std::shared_ptr<const void> binding_;
    friend class MonitoringQ4CpuFreqInventoryAuthority;
    friend class MonitoringQ4CpuFreqSemanticAgreement;
    friend class MonitoringQ4CpuFreqSemanticAgreementTestAccess;
    friend class MonitoringQ4CpuFreqPreparedSourceAuthority;
};

class MonitoringQ4CpuFreqInventoryAuthorityTestAccess;
class MonitoringQ4CpuFreqCycleAuthority;
struct MonitoringQ4CpuFreqEnumerationState final {
    alignas(std::uint64_t) std::array<char, 4096> bytes{};
    std::size_t used{};
    std::size_t cursor{};
};
class MonitoringQ4CpuFreqInventoryAuthority final {
  public:
    MonitoringQ4CpuFreqInventoryAuthority(const MonitoringQ4CpuFreqInventoryAuthority&) = delete;
    MonitoringQ4CpuFreqInventoryAuthority(MonitoringQ4CpuFreqInventoryAuthority&&) noexcept;
    auto operator=(const MonitoringQ4CpuFreqInventoryAuthority&)
        -> MonitoringQ4CpuFreqInventoryAuthority& = delete;
    auto operator=(MonitoringQ4CpuFreqInventoryAuthority&&)
        -> MonitoringQ4CpuFreqInventoryAuthority& = delete;
    ~MonitoringQ4CpuFreqInventoryAuthority() noexcept;

    [[nodiscard]] static auto retain(const MonitoringQ4SourceReady&) noexcept
        -> std::expected<MonitoringQ4CpuFreqInventoryAuthority,
                         MonitoringQ4CpuFreqInventoryFailure>;
    [[nodiscard]] auto capture_and_retire() noexcept
        -> std::expected<MonitoringQ4CpuFreqInventory, MonitoringQ4CpuFreqInventoryFailure>;

  private:
    using PolicyCallback = int (*)(void*, int, std::uint64_t,
                                   const MonitoringQ4CpuFreqPolicyInventory&,
                                   std::span<const char>) noexcept;
    struct Operations final {
        void* context{};
        int (*open_root)(void*, const char*, int) noexcept {};
        int (*open_at)(void*, int, const char*, int) noexcept {};
        int (*stat_fd)(void*, int, struct stat*) noexcept {};
        int (*stat_at)(void*, int, const char*, struct stat*) noexcept {};
        int (*next)(void*, int, MonitoringQ4CpuFreqEnumerationState*, char*,
                    std::size_t) noexcept {};
        ssize_t (*read)(void*, int, void*, std::size_t, std::int64_t) noexcept {};
        int (*close)(void*, int) noexcept {};
        void (*fault)(void*, unsigned){};
        int (*duplicate)(void*, int) noexcept {};
        void* cleanup_context{};
        void (*observe_cleanup)(void*, unsigned, std::uint64_t, std::uint64_t, int,
                                bool) noexcept {};
        uid_t (*effective_uid)(void*) noexcept {};
    };
    struct Storage final {
        std::vector<MonitoringQ4CpuFreqPolicyInventory> policies;
        std::vector<char> contents;
        std::vector<char> root_names;
        std::vector<MonitoringQ4CpuFreqCleanup> cleanup;
        std::size_t cleanup_limit{};
    };
    struct Workspace final {
        Storage storage;
    };
    MonitoringQ4CpuFreqInventoryAuthority(int, struct stat, Operations, Storage,
                                          MonitoringQ4StructuralCapacity) noexcept;
    static auto retain_with(const MonitoringQ4CollectionCapacityInputs&, Operations) noexcept
        -> std::expected<MonitoringQ4CpuFreqInventoryAuthority,
                         MonitoringQ4CpuFreqInventoryFailure>;
    static auto prepare_workspace(const MonitoringQ4CollectionCapacityInputs&, Operations) noexcept
        -> std::expected<Workspace, MonitoringQ4CpuFreqInventoryFailure>;
    static auto retain_prepared(const MonitoringQ4CollectionCapacityInputs&, Operations,
                                Workspace&&) noexcept
        -> std::expected<MonitoringQ4CpuFreqInventoryAuthority,
                         MonitoringQ4CpuFreqInventoryFailure>;
    static auto native_operations() noexcept -> Operations;
    static void bind_ready(MonitoringQ4CpuFreqInventoryAuthority&,
                           const MonitoringQ4SourceReady&) noexcept;
    [[nodiscard]] auto capture_with(void*, PolicyCallback) noexcept
        -> std::expected<MonitoringQ4CpuFreqInventory, MonitoringQ4CpuFreqInventoryFailure>;
    auto fail(MonitoringQ4CpuFreqInventoryFailureKind, int = 0, bool = false,
              MonitoringQ4CpuFreqRole = MonitoringQ4CpuFreqRole::none, std::uint64_t = 0,
              std::uint64_t = 0, MonitoringQ4CapacityReason = MonitoringQ4CapacityReason::none) noexcept
        -> std::expected<MonitoringQ4CpuFreqInventory, MonitoringQ4CpuFreqInventoryFailure>;
    void record_close(MonitoringQ4CpuFreqRole, std::uint64_t, std::uint64_t, int) noexcept;
    void observe_close(MonitoringQ4CpuFreqRole, std::uint64_t, std::uint64_t, int) noexcept;
    int root_{-1};
    struct stat root_identity_{};
    Operations operations_{};
    Storage storage_{};
    MonitoringQ4StructuralCapacity capacity_;
    MonitoringQ4CpuFreqEnumerationState enumeration_{};
    bool terminal_{};
    bool cleanup_overflowed_{};
    std::shared_ptr<const void> binding_;
    friend class MonitoringQ4CpuFreqInventoryAuthorityTestAccess;
    friend class MonitoringQ4CpuFreqPreparedSourceAuthority;
    friend class MonitoringQ4CpuFreqPreparedSourceAuthorityTestAccess;
    friend class MonitoringQ4CpuFreqCycleAuthority;
    friend class MonitoringQ4CpuFreqCollector;
};

} // namespace epycbench::monitoring::qualification::detail
